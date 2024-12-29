/*
 * IO Work Queue Simulation
 * 
 * This program simulates the io_uring work queue system based on
 * the Linux kernel's implementation. It provides a user-space simulation
 * of worker thread pools for handling asynchronous I/O operations.
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
#include <assert.h>
#include <sys/time.h>

/* Configuration Constants */
#define IO_WQ_MAX_WORKERS     32
#define IO_WQ_HASH_ORDER      6
#define IO_WQ_HASH_BUCKETS    (1U << IO_WQ_HASH_ORDER)
#define WORKER_IDLE_TIMEOUT   5000  /* 5 seconds */

/* Worker Flags */
enum {
    IO_WORKER_F_UP       = 1,    /* up and active */
    IO_WORKER_F_RUNNING  = 2,    /* account as running */
    IO_WORKER_F_FREE     = 4,    /* worker on free list */
    IO_WORKER_F_BOUND    = 8,    /* is doing bounded work */
    IO_WORKER_F_EXITING  = 16    /* worker is exiting */
};

/* Work Queue Flags */
enum {
    IO_WQ_F_EXIT        = 1,    /* work queue exiting */
    IO_WQ_F_STALLED     = 2,    /* stalled on hash */
};

/* Work Item Flags */
enum {
    IO_WQ_WORK_UNBOUND  = 1,    /* not bound to specific CPU */
    IO_WQ_WORK_HASHED   = 2,    /* uses hash to serialize */
    IO_WQ_WORK_CANCEL   = 4,    /* cancel in progress */
};

/* Forward Declarations */
struct io_worker;
struct io_wq;
struct io_wq_work;

/* Work Function Types */
typedef void (*io_wq_work_fn)(struct io_wq_work *);
typedef void (*free_work_fn)(struct io_wq_work *);
typedef bool (*cancel_work_fn)(struct io_wq_work *, void *);

/* Work Item Structure */
struct io_wq_work {
    struct io_wq_work *next;
    io_wq_work_fn work_fn;
    void *data;
    unsigned int flags;
    unsigned int hash;
    int error;
};

/* Work List Structure */
struct io_wq_work_list {
    struct io_wq_work *first;
    struct io_wq_work *last;
};

/* Worker Thread Structure */
struct io_worker {
    pthread_t thread;
    unsigned int flags;
    struct io_wq *wq;
    struct io_wq_work *current_work;
    time_t last_active;
    
    /* Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    
    /* Statistics */
    unsigned long completed_work;
    unsigned long failed_work;
    
    /* Linked list pointers */
    struct io_worker *next_free;
    struct io_worker *prev_free;
    struct io_worker *next_all;
    struct io_worker *prev_all;
};

/* Work Queue Accounting Structure */
struct io_wq_acct {
    unsigned int nr_workers;
    unsigned int max_workers;
    atomic_int nr_running;
    pthread_mutex_t lock;
    struct io_wq_work_list work_list;
    unsigned long flags;
};

/* Main Work Queue Structure */
struct io_wq {
    unsigned long flags;
    
    /* Work handlers */
    free_work_fn *free_work;
    io_wq_work_fn *do_work;
    
    /* Worker management */
    struct io_worker *workers;
    unsigned int nr_workers;
    unsigned int max_workers;
    
    /* Free worker list */
    struct io_worker *free_list;
    pthread_mutex_t free_list_lock;
    
    /* Work distribution */
    struct io_wq_work_list work_list;
    struct io_wq_work_list *hash_lists[IO_WQ_HASH_BUCKETS];
    pthread_mutex_t hash_lock[IO_WQ_HASH_BUCKETS];
    
    /* Statistics */
    atomic_long total_work_items;
    atomic_long completed_work_items;
    atomic_int active_workers;
    
    /* Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    
    /* Accounting */
    struct io_wq_acct bounded_acct;
    struct io_wq_acct unbounded_acct;
};

/* Helper Functions */

static unsigned int get_work_hash(struct io_wq_work *work) {
    return work->hash & (IO_WQ_HASH_BUCKETS - 1);
}

static void io_wq_work_list_add(struct io_wq_work_list *list, 
                               struct io_wq_work *work) {
    work->next = NULL;
    if (!list->first) {
        list->first = list->last = work;
    } else {
        list->last->next = work;
        list->last = work;
    }
}

static struct io_wq_work *io_wq_work_list_get(struct io_wq_work_list *list) {
    struct io_wq_work *work = list->first;
    
    if (work) {
        list->first = work->next;
        if (!list->first)
            list->last = NULL;
        work->next = NULL;
    }
    
    return work;
}

/* Worker Thread Functions */

static void io_worker_exit(struct io_worker *worker) {
    struct io_wq *wq = worker->wq;
    
    pthread_mutex_lock(&wq->lock);
    worker->flags |= IO_WORKER_F_EXITING;
    atomic_fetch_sub(&wq->active_workers, 1);
    pthread_mutex_unlock(&wq->lock);
    
    pthread_exit(NULL);
}

static struct io_wq_work *io_worker_get_work(struct io_worker *worker) {
    struct io_wq *wq = worker->wq;
    struct io_wq_work *work = NULL;
    unsigned int hash;
    
    /* First check hash lists */
    for (hash = 0; hash < IO_WQ_HASH_BUCKETS; hash++) {
        pthread_mutex_lock(&wq->hash_lock[hash]);
        work = io_wq_work_list_get(wq->hash_lists[hash]);
        pthread_mutex_unlock(&wq->hash_lock[hash]);
        if (work)
            break;
    }
    
    /* If no hashed work, check main list */
    if (!work) {
        pthread_mutex_lock(&wq->lock);
        work = io_wq_work_list_get(&wq->work_list);
        pthread_mutex_unlock(&wq->lock);
    }
    
    return work;
}

static void *io_worker_thread(void *data) {
    struct io_worker *worker = (struct io_worker *)data;
    struct io_wq *wq = worker->wq;
    struct io_wq_work *work;
    
    worker->flags |= IO_WORKER_F_UP;
    atomic_fetch_add(&wq->active_workers, 1);
    
    while (!(worker->flags & IO_WORKER_F_EXITING)) {
        /* Get work item */
        work = io_worker_get_work(worker);
        
        if (work) {
            /* Process work item */
            worker->current_work = work;
            worker->flags |= IO_WORKER_F_RUNNING;
            worker->last_active = time(NULL);
            
            if (work->work_fn) {
                work->work_fn(work);
                worker->completed_work++;
                atomic_fetch_add(&wq->completed_work_items, 1);
            }
            
            worker->current_work = NULL;
            worker->flags &= ~IO_WORKER_F_RUNNING;
            
            /* Free work item */
            if (wq->free_work)
                wq->free_work(work);
            else
                free(work);
            
        } else {
            /* No work available, wait for condition or timeout */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += WORKER_IDLE_TIMEOUT / 1000;
            
            pthread_mutex_lock(&worker->lock);
            pthread_cond_timedwait(&worker->cond, &worker->lock, &ts);
            pthread_mutex_unlock(&worker->lock);
            
            /* Check if we should exit due to being idle too long */
            if (time(NULL) - worker->last_active > WORKER_IDLE_TIMEOUT / 1000) {
                if (atomic_load(&wq->active_workers) > wq->max_workers / 2)
                    break;
            }
        }
    }
    
    io_worker_exit(worker);
    return NULL;
}

/* Work Queue Management Functions */

static struct io_worker *io_wq_create_worker(struct io_wq *wq) {
    struct io_worker *worker;
    
    worker = calloc(1, sizeof(*worker));
    if (!worker)
        return NULL;
    
    worker->wq = wq;
    worker->flags = 0;
    worker->last_active = time(NULL);
    
    pthread_mutex_init(&worker->lock, NULL);
    pthread_cond_init(&worker->cond, NULL);
    
    if (pthread_create(&worker->thread, NULL, io_worker_thread, worker) != 0) {
        pthread_mutex_destroy(&worker->lock);
        pthread_cond_destroy(&worker->cond);
        free(worker);
        return NULL;
    }
    
    return worker;
}

static void io_wq_add_worker(struct io_wq *wq, struct io_worker *worker) {
    pthread_mutex_lock(&wq->lock);
    
    /* Add to all workers list */
    worker->next_all = wq->workers;
    if (wq->workers)
        wq->workers->prev_all = worker;
    wq->workers = worker;
    wq->nr_workers++;
    
    pthread_mutex_unlock(&wq->lock);
}

struct io_wq *io_wq_create(unsigned int max_workers) {
    struct io_wq *wq;
    int i;
    
    wq = calloc(1, sizeof(*wq));
    if (!wq)
        return NULL;
    
    /* Initialize work queue */
    wq->max_workers = max_workers;
    atomic_init(&wq->total_work_items, 0);
    atomic_init(&wq->completed_work_items, 0);
    atomic_init(&wq->active_workers, 0);
    
    pthread_mutex_init(&wq->lock, NULL);
    pthread_cond_init(&wq->cond, NULL);
    pthread_mutex_init(&wq->free_list_lock, NULL);
    
    /* Initialize hash lists */
    for (i = 0; i < IO_WQ_HASH_BUCKETS; i++) {
        wq->hash_lists[i] = calloc(1, sizeof(struct io_wq_work_list));
        pthread_mutex_init(&wq->hash_lock[i], NULL);
    }
    
    /* Create initial workers */
    for (i = 0; i < max_workers / 2; i++) {
        struct io_worker *worker = io_wq_create_worker(wq);
        if (worker)
            io_wq_add_worker(wq, worker);
    }
    
    return wq;
}

void io_wq_destroy(struct io_wq *wq) {
    struct io_worker *worker;
    int i;
    
    /* Signal all workers to exit */
    wq->flags |= IO_WQ_F_EXIT;
    
    pthread_mutex_lock(&wq->lock);
    worker = wq->workers;
    while (worker) {
        worker->flags |= IO_WORKER_F_EXITING;
        pthread_cond_signal(&worker->cond);
        worker = worker->next_all;
    }
    pthread_mutex_unlock(&wq->lock);
    
    /* Wait for all workers to exit */
    worker = wq->workers;
    while (worker) {
        struct io_worker *next = worker->next_all;
        pthread_join(worker->thread, NULL);
        pthread_mutex_destroy(&worker->lock);
        pthread_cond_destroy(&worker->cond);
        free(worker);
        worker = next;
    }
    
    /* Free hash lists */
    for (i = 0; i < IO_WQ_HASH_BUCKETS; i++) {
        pthread_mutex_destroy(&wq->hash_lock[i]);
        free(wq->hash_lists[i]);
    }
    
    pthread_mutex_destroy(&wq->lock);
    pthread_cond_destroy(&wq->cond);
    pthread_mutex_destroy(&wq->free_list_lock);
    
    free(wq);
}

int io_wq_enqueue(struct io_wq *wq, struct io_wq_work *work) {
    unsigned int hash;
    
    if (!work)
        return -EINVAL;
    
    atomic_fetch_add(&wq->total_work_items, 1);
    
    if (work->flags & IO_WQ_WORK_HASHED) {
        hash = get_work_hash(work);
        pthread_mutex_lock(&wq->hash_lock[hash]);
        io_wq_work_list_add(wq->hash_lists[hash], work);
        pthread_mutex_unlock(&wq->hash_lock[hash]);
    } else {
        pthread_mutex_lock(&wq->lock);
        io_wq_work_list_add(&wq->work_list, work);
        pthread_mutex_unlock(&wq->lock);
    }
    
    /* Wake up a worker if needed */
    if (atomic_load(&wq->active_workers) < wq->nr_workers) {
        worker = wq->workers;
        while (worker) {
            if (!(worker->flags & IO_WORKER_F_RUNNING)) {
                pthread_cond_signal(&worker->cond);
                break;
            }
            worker = worker->next_all;
        }
    }
    
    return 0;
}

/* Example Usage and Testing */

void example_work_fn(struct io_wq_work *work) {
    printf("Processing work item %p with data %p\n", work, work->data);
    usleep(100000); /* Simulate some work */
}

void example_free_fn(struct io_wq_work *work) {
    free(work->data);
    free(work);
}

int main(void) {
    struct io_wq *wq;
    struct io_wq_work *work;
    int i, ret;
    const int NUM_WORK_ITEMS = 100;
    
    printf("IO Work Queue Simulation\n");
    printf("=======================\n\n");
    
    /* Create work queue */
    wq = io_wq_create(8);
    if (!wq) {
        fprintf(stderr, "Failed to create work queue\n");
        return 1;
    }
    
    /* Set work handlers */
    wq->free_work = example_free_fn;
    wq->do_work = example_work_fn;
    
    printf("Submitting %d work items...\n", NUM_WORK_ITEMS);
    
    /* Submit work items */
    for (i = 0; i < NUM_WORK_ITEMS; i++) {
        work = calloc(1, sizeof(*work));
        if (!work)
            continue;
        
        work->data = malloc(64); /* Some dummy data */
        work->work_fn = example_work_fn;
        work->hash = i % 16; /* Some items will hash to same bucket */
        work->flags = (i % 2) ? IO_WQ_WORK_HASHED : 0;
        
        ret = io_wq_enqueue(wq, work);
        if (ret < 0) {
            fprintf(stderr, "Failed to enqueue work item %d\n", i);
            example_free_fn(work);
            continue;
        }
    }
    
    /* Wait for work to complete */
    while (atomic_load(&wq->completed_work_items) < NUM_WORK_ITEMS) {
        printf("Progress: %ld/%d work items completed\n",
               atomic_load(&wq->completed_work_items), NUM_WORK_ITEMS);
        sleep(1);
    }
    
    printf("\nFinal Statistics:\n");
    printf("Total work items: %ld\n", atomic_load(&wq->total_work_items));
    printf("Completed work items: %ld\n", atomic_load(&wq->completed_work_items));
    printf("Active workers: %d\n", atomic_load(&wq->active_workers));
    
    /* Cleanup */
    io_wq_destroy(wq);
    
    return 0;
}
