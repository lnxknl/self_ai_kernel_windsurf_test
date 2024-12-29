#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mq_deadline_standalone.h"

/* Default configuration values */
#define DEFAULT_READ_EXPIRE    500  /* 500ms */
#define DEFAULT_WRITE_EXPIRE   5000 /* 5000ms */
#define DEFAULT_BATCH_COUNT    16   /* 16 requests */
#define DEFAULT_WRITES_STARVED 2    /* 2 read batches */

/* Helper function to create a new request */
static struct request *create_request(uint64_t sector, uint32_t nr_sectors,
                                    enum dd_data_dir dir, enum dd_prio prio) {
    struct request *rq = malloc(sizeof(*rq));
    if (!rq)
        return NULL;

    rq->sector = sector;
    rq->nr_sectors = nr_sectors;
    rq->dir = dir;
    rq->prio = prio;
    rq->submit_time = time(NULL);
    rq->next = NULL;
    return rq;
}

/* Add request to FIFO list */
static void add_request_fifo(struct request **head, struct request *rq) {
    struct request **cur = head;
    while (*cur)
        cur = &(*cur)->next;
    *cur = rq;
}

/* Check if request has expired */
static bool request_expired(struct deadline_data *dd, struct request *rq) {
    time_t now = time(NULL);
    int expire = (rq->dir == DD_READ) ? dd->read_expire : dd->write_expire;
    return (now - rq->submit_time) * 1000 >= expire;
}

/* Get first expired request from FIFO */
static struct request *get_expired_request(struct deadline_data *dd,
                                         struct dd_per_prio *per_prio,
                                         enum dd_data_dir dir) {
    struct request *rq = per_prio->fifo[dir];
    
    if (!rq)
        return NULL;

    if (request_expired(dd, rq)) {
        per_prio->fifo[dir] = rq->next;
        rq->next = NULL;
        return rq;
    }

    return NULL;
}

/* Find best request based on sector position */
static struct request *find_best_request(struct deadline_data *dd,
                                       struct dd_per_prio *per_prio,
                                       enum dd_data_dir dir) {
    struct request *rq = per_prio->fifo[dir];
    struct request *best = NULL;
    uint64_t best_distance = UINT64_MAX;
    uint64_t current_pos = per_prio->latest_pos[dir];

    while (rq) {
        uint64_t distance;
        if (rq->sector >= current_pos)
            distance = rq->sector - current_pos;
        else
            distance = current_pos - rq->sector;

        if (distance < best_distance) {
            best_distance = distance;
            best = rq;
        }
        rq = rq->next;
    }

    if (best) {
        /* Remove from FIFO */
        struct request **cur = &per_prio->fifo[dir];
        while (*cur && *cur != best)
            cur = &(*cur)->next;
        if (*cur)
            *cur = best->next;
        best->next = NULL;
    }

    return best;
}

struct deadline_data *deadline_init(void) {
    struct deadline_data *dd = calloc(1, sizeof(*dd));
    if (!dd)
        return NULL;

    dd->read_expire = DEFAULT_READ_EXPIRE;
    dd->write_expire = DEFAULT_WRITE_EXPIRE;
    dd->batch_count = DEFAULT_BATCH_COUNT;
    dd->writes_starved = DEFAULT_WRITES_STARVED;
    dd->last_dir = DD_READ;

    return dd;
}

void deadline_exit(struct deadline_data *dd) {
    if (!dd)
        return;

    /* Free all queued requests */
    for (int p = 0; p < 3; p++) {
        for (int d = 0; d < 2; d++) {
            struct request *rq = dd->per_prio[p].fifo[d];
            while (rq) {
                struct request *next = rq->next;
                free(rq);
                rq = next;
            }
        }
    }

    free(dd);
}

int deadline_add_request(struct deadline_data *dd, uint64_t sector,
                        uint32_t nr_sectors, enum dd_data_dir dir,
                        enum dd_prio prio) {
    struct request *rq;

    if (!dd || prio > DD_IDLE_PRIO)
        return -1;

    rq = create_request(sector, nr_sectors, dir, prio);
    if (!rq)
        return -1;

    add_request_fifo(&dd->per_prio[prio].fifo[dir], rq);
    return 0;
}

struct request *deadline_dispatch_request(struct deadline_data *dd) {
    struct request *rq = NULL;
    enum dd_data_dir data_dir = dd->last_dir;
    int p;

    if (!dd)
        return NULL;

    /* First check for expired requests, starting with highest priority */
    for (p = 0; p < 3 && !rq; p++) {
        struct dd_per_prio *per_prio = &dd->per_prio[p];
        
        /* Check expired reads first */
        rq = get_expired_request(dd, per_prio, DD_READ);
        if (!rq)
            rq = get_expired_request(dd, per_prio, DD_WRITE);
    }

    /* If no expired requests, use sector-based selection */
    if (!rq) {
        /* Switch between reads and writes to prevent starvation */
        if (data_dir == DD_READ) {
            if (dd->starved >= dd->writes_starved) {
                dd->starved = 0;
                data_dir = DD_WRITE;
            }
        }

        for (p = 0; p < 3 && !rq; p++) {
            struct dd_per_prio *per_prio = &dd->per_prio[p];
            rq = find_best_request(dd, per_prio, data_dir);
        }

        if (!rq && data_dir == DD_READ)
            dd->starved++;
    }

    if (rq) {
        dd->last_dir = rq->dir;
        dd->per_prio[rq->prio].stats.dispatched++;
        dd->per_prio[rq->prio].latest_pos[rq->dir] = rq->sector + rq->nr_sectors;
    }

    return rq;
}

void deadline_finish_request(struct deadline_data *dd, struct request *rq) {
    if (!dd || !rq)
        return;

    struct dd_prio_stats *stats = &dd->per_prio[rq->prio].stats;
    time_t now = time(NULL);
    double latency = difftime(now, rq->submit_time);

    stats->completed++;
    stats->avg_latency = ((stats->avg_latency * (stats->completed - 1)) + latency)
                        / stats->completed;

    free(rq);
}

void deadline_get_stats(struct deadline_data *dd, struct dd_prio_stats *stats) {
    if (!dd || !stats)
        return;

    for (int p = 0; p < 3; p++)
        stats[p] = dd->per_prio[p].stats;
}

void deadline_print_status(struct deadline_data *dd) {
    const char *prio_names[] = {"Real-time", "Best-effort", "Idle"};
    const char *dir_names[] = {"Read", "Write"};
    
    if (!dd)
        return;

    printf("\nDeadline Scheduler Status:\n");
    printf("Configuration:\n");
    printf("  Read expire: %d ms\n", dd->read_expire);
    printf("  Write expire: %d ms\n", dd->write_expire);
    printf("  Batch count: %d\n", dd->batch_count);
    printf("  Writes starved: %d/%d\n", dd->starved, dd->writes_starved);

    printf("\nQueue Status:\n");
    for (int p = 0; p < 3; p++) {
        printf("\n%s Priority:\n", prio_names[p]);
        for (int d = 0; d < 2; d++) {
            struct request *rq = dd->per_prio[p].fifo[d];
            int count = 0;
            while (rq) {
                count++;
                rq = rq->next;
            }
            printf("  %s queue: %d requests\n", dir_names[d], count);
        }
        printf("  Statistics:\n");
        printf("    Dispatched: %u\n", dd->per_prio[p].stats.dispatched);
        printf("    Completed: %u\n", dd->per_prio[p].stats.completed);
        printf("    Avg latency: %.2f seconds\n", dd->per_prio[p].stats.avg_latency);
    }
    printf("\n");
}

/* Example usage */
int main(void) {
    struct deadline_data *dd;
    struct request *rq;
    struct dd_prio_stats stats[3];

    /* Initialize scheduler */
    dd = deadline_init();
    if (!dd) {
        printf("Failed to initialize scheduler\n");
        return 1;
    }

    /* Add some test requests */
    deadline_add_request(dd, 100, 8, DD_READ, DD_RT_PRIO);
    deadline_add_request(dd, 200, 8, DD_WRITE, DD_BE_PRIO);
    deadline_add_request(dd, 150, 8, DD_READ, DD_RT_PRIO);
    deadline_add_request(dd, 50, 8, DD_WRITE, DD_IDLE_PRIO);

    /* Print initial status */
    printf("Initial status:\n");
    deadline_print_status(dd);

    /* Process all requests */
    while ((rq = deadline_dispatch_request(dd))) {
        printf("\nDispatching request:\n");
        printf("Sector: %lu, Size: %u, %s, Priority: %d\n",
               rq->sector, rq->nr_sectors,
               rq->dir == DD_READ ? "Read" : "Write",
               rq->prio);
        
        deadline_finish_request(dd, rq);
    }

    /* Get final statistics */
    deadline_get_stats(dd, stats);
    printf("\nFinal statistics:\n");
    for (int p = 0; p < 3; p++) {
        printf("Priority %d:\n", p);
        printf("  Dispatched: %u\n", stats[p].dispatched);
        printf("  Completed: %u\n", stats[p].completed);
        printf("  Avg latency: %.2f seconds\n", stats[p].avg_latency);
    }

    deadline_exit(dd);
    return 0;
}
