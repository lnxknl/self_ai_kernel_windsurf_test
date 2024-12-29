#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "elevator_standalone.h"

/* Helper function to create a new request */
static struct request *create_request(uint64_t sector, uint32_t nr_sectors, 
                                   enum req_dir dir) {
    struct request *rq = malloc(sizeof(*rq));
    if (!rq)
        return NULL;

    rq->sector = sector;
    rq->nr_sectors = nr_sectors;
    rq->dir = dir;
    rq->submit_time = time(NULL);
    rq->next = NULL;
    return rq;
}

/* NOOP scheduler - simple FIFO */
static void noop_add_request(struct elevator_queue *e, struct request *rq) {
    struct request **cur = &e->queue;
    while (*cur)
        cur = &(*cur)->next;
    *cur = rq;
}

static struct request *noop_next_request(struct elevator_queue *e) {
    return e->queue;
}

/* SCAN (elevator) scheduler */
static void scan_add_request(struct elevator_queue *e, struct request *rq) {
    struct request **cur = &e->queue;
    struct request *prev = NULL;

    /* Insert in sorted order by sector */
    while (*cur && (*cur)->sector < rq->sector) {
        prev = *cur;
        cur = &(*cur)->next;
    }
    
    rq->next = *cur;
    *cur = rq;
}

static struct request *scan_next_request(struct elevator_queue *e) {
    struct request *rq = e->queue;
    struct request *best = NULL;
    uint64_t best_distance = UINT64_MAX;

    /* Find closest request to current head position */
    while (rq) {
        uint64_t distance;
        if (rq->sector >= e->current_sector)
            distance = rq->sector - e->current_sector;
        else
            distance = e->current_sector - rq->sector;
            
        if (distance < best_distance) {
            best_distance = distance;
            best = rq;
        }
        rq = rq->next;
    }
    
    return best;
}

/* Deadline scheduler */
static void deadline_add_request(struct elevator_queue *e, struct request *rq) {
    /* Simple implementation: prioritize reads over writes */
    struct request **cur = &e->queue;
    
    if (rq->dir == REQ_READ) {
        /* Insert read at front */
        rq->next = e->queue;
        e->queue = rq;
    } else {
        /* Insert write at end */
        while (*cur)
            cur = &(*cur)->next;
        *cur = rq;
    }
}

static struct request *deadline_next_request(struct elevator_queue *e) {
    struct request *rq = e->queue;
    struct request *oldest = rq;
    time_t oldest_time = rq ? rq->submit_time : 0;

    /* Find request with earliest submit time */
    while (rq) {
        if (rq->submit_time < oldest_time) {
            oldest_time = rq->submit_time;
            oldest = rq;
        }
        rq = rq->next;
    }
    
    return oldest;
}

struct elevator_queue *elevator_init(enum elevator_type type) {
    struct elevator_queue *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
        
    e->type = type;
    e->current_sector = 0;
    return e;
}

void elevator_exit(struct elevator_queue *e) {
    struct request *rq = e->queue;
    while (rq) {
        struct request *next = rq->next;
        free(rq);
        rq = next;
    }
    free(e);
}

int elevator_add_request(struct elevator_queue *e, uint64_t sector,
                        uint32_t nr_sectors, enum req_dir dir) {
    struct request *rq = create_request(sector, nr_sectors, dir);
    if (!rq)
        return -1;

    switch (e->type) {
    case ELEVATOR_NOOP:
        noop_add_request(e, rq);
        break;
    case ELEVATOR_SCAN:
        scan_add_request(e, rq);
        break;
    case ELEVATOR_DEADLINE:
        deadline_add_request(e, rq);
        break;
    }

    e->nr_requests++;
    e->total_requests++;
    e->total_sectors += nr_sectors;
    return 0;
}

struct request *elevator_next_request(struct elevator_queue *e) {
    if (!e->queue)
        return NULL;

    switch (e->type) {
    case ELEVATOR_NOOP:
        return noop_next_request(e);
    case ELEVATOR_SCAN:
        return scan_next_request(e);
    case ELEVATOR_DEADLINE:
        return deadline_next_request(e);
    }

    return NULL;
}

void elevator_complete_request(struct elevator_queue *e, struct request *rq) {
    struct request **cur = &e->queue;
    time_t now = time(NULL);
    double latency;

    /* Find and remove request from queue */
    while (*cur && *cur != rq)
        cur = &(*cur)->next;
    
    if (*cur) {
        *cur = rq->next;
        e->nr_requests--;
        
        /* Update statistics */
        latency = difftime(now, rq->submit_time);
        e->avg_latency = ((e->avg_latency * (e->total_requests - 1)) + latency) 
                        / e->total_requests;
        
        /* Update current head position */
        e->current_sector = rq->sector + rq->nr_sectors;
        
        free(rq);
    }
}

void elevator_get_stats(struct elevator_queue *e, uint64_t *total_reqs,
                       uint64_t *total_sectors, double *avg_latency) {
    if (total_reqs)
        *total_reqs = e->total_requests;
    if (total_sectors)
        *total_sectors = e->total_sectors;
    if (avg_latency)
        *avg_latency = e->avg_latency;
}

void elevator_print_status(struct elevator_queue *e) {
    struct request *rq = e->queue;
    const char *type_str[] = {"NOOP", "DEADLINE", "SCAN"};

    printf("\nElevator Status:\n");
    printf("Algorithm: %s\n", type_str[e->type]);
    printf("Current sector: %lu\n", e->current_sector);
    printf("Requests in queue: %d\n", e->nr_requests);
    printf("Total requests: %lu\n", e->total_requests);
    printf("Total sectors: %lu\n", e->total_sectors);
    printf("Average latency: %.2f seconds\n", e->avg_latency);
    
    printf("\nRequest Queue:\n");
    while (rq) {
        printf("Sector: %lu, Size: %u, Type: %s\n",
               rq->sector, rq->nr_sectors,
               rq->dir == REQ_READ ? "READ" : "WRITE");
        rq = rq->next;
    }
    printf("\n");
}

/* Example usage */
int main(void) {
    struct elevator_queue *e;
    struct request *rq;
    uint64_t total_reqs, total_sectors;
    double avg_latency;

    /* Test each scheduler type */
    for (enum elevator_type type = ELEVATOR_NOOP; 
         type <= ELEVATOR_SCAN; type++) {
        
        printf("\nTesting %s scheduler:\n",
               type == ELEVATOR_NOOP ? "NOOP" :
               type == ELEVATOR_DEADLINE ? "Deadline" : "SCAN");

        e = elevator_init(type);
        if (!e) {
            printf("Failed to initialize elevator\n");
            continue;
        }

        /* Add some test requests */
        elevator_add_request(e, 100, 8, REQ_READ);
        elevator_add_request(e, 50, 16, REQ_WRITE);
        elevator_add_request(e, 200, 8, REQ_READ);
        elevator_add_request(e, 75, 8, REQ_READ);

        /* Process all requests */
        while ((rq = elevator_next_request(e))) {
            printf("Processing request: sector %lu, size %u, type %s\n",
                   rq->sector, rq->nr_sectors,
                   rq->dir == REQ_READ ? "READ" : "WRITE");
            elevator_complete_request(e, rq);
        }

        /* Get final statistics */
        elevator_get_stats(e, &total_reqs, &total_sectors, &avg_latency);
        printf("\nFinal statistics:\n");
        printf("Total requests: %lu\n", total_reqs);
        printf("Total sectors: %lu\n", total_sectors);
        printf("Average latency: %.2f seconds\n", avg_latency);

        elevator_exit(e);
    }

    return 0;
}
