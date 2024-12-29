/*
 * ARM SMP (Symmetric Multi-Processing) Simulation
 * 
 * This program simulates the ARM SMP subsystem, including CPU
 * boot-up, inter-processor interrupts, and cross-CPU operations.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

/* Configuration Constants */
#define ARM_MAX_CPUS        8
#define ARM_MAX_IPI_MSGS    32
#define ARM_IPI_TIMEOUT     1000  /* milliseconds */
#define ARM_CPU_BOOT_TIMEOUT 5000 /* milliseconds */
#define ARM_MAX_CACHE_LINES 1024
#define ARM_CACHE_LINE_SIZE 64
#define ARM_L1_CACHE_SIZE   32768 /* 32KB */
#define ARM_L2_CACHE_SIZE   262144 /* 256KB */

/* CPU States */
#define CPU_OFFLINE         0
#define CPU_ONLINE          1
#define CPU_DEAD           2
#define CPU_DYING          3

/* IPI Types */
#define IPI_RESCHEDULE     0
#define IPI_CALL_FUNC      1
#define IPI_CALL_FUNC_SINGLE 2
#define IPI_CPU_STOP       3
#define IPI_TIMER          4
#define IPI_IRQ_WORK       5
#define IPI_WAKEUP         6
#define MAX_IPI_TYPES      7

/* Cache States */
#define CACHE_INVALID      0
#define CACHE_SHARED       1
#define CACHE_EXCLUSIVE    2
#define CACHE_MODIFIED     3

/* CPU Structure */
struct arm_cpu {
    uint32_t cpu_id;       /* CPU ID */
    uint32_t state;        /* CPU state */
    uint32_t online_count; /* Times brought online */
    uint64_t last_tick;    /* Last tick count */
    void    *stack;        /* CPU stack */
    pthread_t thread;      /* CPU thread */
    pthread_mutex_t lock;  /* CPU lock */
    pthread_cond_t wait;   /* CPU wait condition */
    bool     has_fpu;      /* FPU present */
    bool     has_neon;     /* NEON present */
};

/* IPI Message Structure */
struct ipi_msg {
    uint32_t type;         /* IPI type */
    uint32_t src_cpu;      /* Source CPU */
    uint32_t dst_cpu;      /* Destination CPU */
    void    *data;         /* Message data */
    void   (*func)(void *);/* Function to call */
    struct ipi_msg *next;  /* Next message */
};

/* IPI Queue Structure */
struct ipi_queue {
    struct ipi_msg *head;  /* Queue head */
    struct ipi_msg *tail;  /* Queue tail */
    uint32_t count;        /* Message count */
    pthread_mutex_t lock;  /* Queue lock */
    pthread_cond_t wait;   /* Wait condition */
};

/* Cache Line Structure */
struct cache_line {
    uint64_t addr;         /* Line address */
    uint32_t state;        /* Cache state */
    uint8_t  data[ARM_CACHE_LINE_SIZE]; /* Line data */
    bool     valid;        /* Valid flag */
};

/* Cache Structure */
struct cpu_cache {
    struct cache_line l1d[ARM_MAX_CACHE_LINES]; /* L1 Data cache */
    struct cache_line l1i[ARM_MAX_CACHE_LINES]; /* L1 Instruction cache */
    struct cache_line l2[ARM_MAX_CACHE_LINES];  /* L2 cache */
    pthread_mutex_t lock;  /* Cache lock */
};

/* Global Variables */
static struct arm_cpu *cpus[ARM_MAX_CPUS];
static struct ipi_queue *ipi_queues[ARM_MAX_CPUS];
static struct cpu_cache *cpu_caches[ARM_MAX_CPUS];
static uint32_t nr_cpus = 0;
static bool system_running = false;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

/* Function Prototypes */
static struct arm_cpu *alloc_cpu(uint32_t cpu_id);
static void free_cpu(struct arm_cpu *cpu);
static struct ipi_queue *alloc_ipi_queue(void);
static void free_ipi_queue(struct ipi_queue *queue);
static struct cpu_cache *alloc_cpu_cache(void);
static void free_cpu_cache(struct cpu_cache *cache);
static int send_ipi_msg(uint32_t src_cpu, uint32_t dst_cpu,
                       uint32_t type, void *data,
                       void (*func)(void *));

/* CPU Operations */

static struct arm_cpu *alloc_cpu(uint32_t cpu_id) {
    struct arm_cpu *cpu;
    
    cpu = calloc(1, sizeof(*cpu));
    if (!cpu)
        return NULL;
    
    cpu->cpu_id = cpu_id;
    cpu->state = CPU_OFFLINE;
    cpu->stack = calloc(1, 8192); /* 8KB stack */
    if (!cpu->stack) {
        free(cpu);
        return NULL;
    }
    
    pthread_mutex_init(&cpu->lock, NULL);
    pthread_cond_init(&cpu->wait, NULL);
    
    /* Simulate CPU features */
    cpu->has_fpu = true;
    cpu->has_neon = true;
    
    return cpu;
}

static void free_cpu(struct arm_cpu *cpu) {
    if (!cpu)
        return;
    
    free(cpu->stack);
    pthread_mutex_destroy(&cpu->lock);
    pthread_cond_destroy(&cpu->wait);
    free(cpu);
}

/* IPI Queue Operations */

static struct ipi_queue *alloc_ipi_queue(void) {
    struct ipi_queue *queue;
    
    queue = calloc(1, sizeof(*queue));
    if (!queue)
        return NULL;
    
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->wait, NULL);
    
    return queue;
}

static void free_ipi_queue(struct ipi_queue *queue) {
    struct ipi_msg *msg, *next;
    
    if (!queue)
        return;
    
    pthread_mutex_lock(&queue->lock);
    msg = queue->head;
    while (msg) {
        next = msg->next;
        free(msg);
        msg = next;
    }
    pthread_mutex_unlock(&queue->lock);
    
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->wait);
    free(queue);
}

/* Cache Operations */

static struct cpu_cache *alloc_cpu_cache(void) {
    struct cpu_cache *cache;
    
    cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;
    
    pthread_mutex_init(&cache->lock, NULL);
    return cache;
}

static void free_cpu_cache(struct cpu_cache *cache) {
    if (!cache)
        return;
    
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

/* IPI Message Handling */

static int send_ipi_msg(uint32_t src_cpu, uint32_t dst_cpu,
                       uint32_t type, void *data,
                       void (*func)(void *)) {
    struct ipi_queue *queue;
    struct ipi_msg *msg;
    
    if (dst_cpu >= nr_cpus)
        return -EINVAL;
    
    queue = ipi_queues[dst_cpu];
    if (!queue)
        return -EINVAL;
    
    msg = calloc(1, sizeof(*msg));
    if (!msg)
        return -ENOMEM;
    
    msg->type = type;
    msg->src_cpu = src_cpu;
    msg->dst_cpu = dst_cpu;
    msg->data = data;
    msg->func = func;
    
    pthread_mutex_lock(&queue->lock);
    
    if (!queue->head)
        queue->head = msg;
    else
        queue->tail->next = msg;
    queue->tail = msg;
    queue->count++;
    
    pthread_cond_signal(&queue->wait);
    pthread_mutex_unlock(&queue->lock);
    
    return 0;
}

static void process_ipi_msg(struct arm_cpu *cpu,
                          struct ipi_msg *msg) {
    printf("CPU%u: Processing IPI from CPU%u: type=%u\n",
           cpu->cpu_id, msg->src_cpu, msg->type);
    
    switch (msg->type) {
    case IPI_RESCHEDULE:
        /* Simulate reschedule */
        break;
        
    case IPI_CALL_FUNC:
        if (msg->func)
            msg->func(msg->data);
        break;
        
    case IPI_CPU_STOP:
        pthread_mutex_lock(&cpu->lock);
        cpu->state = CPU_DYING;
        pthread_mutex_unlock(&cpu->lock);
        break;
        
    default:
        printf("CPU%u: Unknown IPI type: %u\n",
               cpu->cpu_id, msg->type);
        break;
    }
}

/* Cache Coherency */

static void invalidate_cache_line(struct cpu_cache *cache,
                                uint64_t addr) {
    uint32_t index = (addr / ARM_CACHE_LINE_SIZE) % ARM_MAX_CACHE_LINES;
    
    pthread_mutex_lock(&cache->lock);
    
    if (cache->l1d[index].valid &&
        cache->l1d[index].addr == addr) {
        cache->l1d[index].valid = false;
        cache->l1d[index].state = CACHE_INVALID;
    }
    
    if (cache->l2[index].valid &&
        cache->l2[index].addr == addr) {
        cache->l2[index].valid = false;
        cache->l2[index].state = CACHE_INVALID;
    }
    
    pthread_mutex_unlock(&cache->lock);
}

static void flush_cache_line(struct cpu_cache *cache,
                           uint64_t addr) {
    uint32_t index = (addr / ARM_CACHE_LINE_SIZE) % ARM_MAX_CACHE_LINES;
    
    pthread_mutex_lock(&cache->lock);
    
    if (cache->l1d[index].valid &&
        cache->l1d[index].addr == addr &&
        cache->l1d[index].state == CACHE_MODIFIED) {
        /* Write back to next level */
        memcpy(cache->l2[index].data,
               cache->l1d[index].data,
               ARM_CACHE_LINE_SIZE);
        cache->l2[index].valid = true;
        cache->l2[index].addr = addr;
        cache->l2[index].state = CACHE_MODIFIED;
        
        cache->l1d[index].state = CACHE_SHARED;
    }
    
    pthread_mutex_unlock(&cache->lock);
}

/* CPU Thread */

static void *cpu_thread(void *data) {
    struct arm_cpu *cpu = data;
    struct ipi_queue *queue = ipi_queues[cpu->cpu_id];
    struct ipi_msg *msg;
    
    printf("CPU%u: Starting...\n", cpu->cpu_id);
    
    pthread_mutex_lock(&cpu->lock);
    cpu->state = CPU_ONLINE;
    cpu->online_count++;
    pthread_mutex_unlock(&cpu->lock);
    
    while (system_running) {
        pthread_mutex_lock(&queue->lock);
        
        while (queue->count == 0 && system_running) {
            pthread_cond_wait(&queue->wait, &queue->lock);
        }
        
        if (!system_running) {
            pthread_mutex_unlock(&queue->lock);
            break;
        }
        
        /* Get next message */
        msg = queue->head;
        queue->head = msg->next;
        if (!queue->head)
            queue->tail = NULL;
        queue->count--;
        
        pthread_mutex_unlock(&queue->lock);
        
        /* Process message */
        process_ipi_msg(cpu, msg);
        free(msg);
        
        /* Simulate some work */
        usleep(1000);
    }
    
    pthread_mutex_lock(&cpu->lock);
    cpu->state = CPU_OFFLINE;
    pthread_mutex_unlock(&cpu->lock);
    
    printf("CPU%u: Stopped\n", cpu->cpu_id);
    return NULL;
}

/* System Initialization */

static int init_system(uint32_t num_cpus) {
    uint32_t i;
    
    if (num_cpus > ARM_MAX_CPUS)
        num_cpus = ARM_MAX_CPUS;
    
    nr_cpus = num_cpus;
    
    for (i = 0; i < nr_cpus; i++) {
        /* Allocate CPU */
        cpus[i] = alloc_cpu(i);
        if (!cpus[i])
            goto cleanup_cpus;
        
        /* Allocate IPI queue */
        ipi_queues[i] = alloc_ipi_queue();
        if (!ipi_queues[i])
            goto cleanup_queues;
        
        /* Allocate cache */
        cpu_caches[i] = alloc_cpu_cache();
        if (!cpu_caches[i])
            goto cleanup_caches;
    }
    
    system_running = true;
    
    /* Start CPU threads */
    for (i = 0; i < nr_cpus; i++) {
        pthread_create(&cpus[i]->thread, NULL,
                      cpu_thread, cpus[i]);
    }
    
    return 0;
    
cleanup_caches:
    while (i--) {
        if (cpu_caches[i])
            free_cpu_cache(cpu_caches[i]);
    }
    i = nr_cpus;
    
cleanup_queues:
    while (i--) {
        if (ipi_queues[i])
            free_ipi_queue(ipi_queues[i]);
    }
    i = nr_cpus;
    
cleanup_cpus:
    while (i--) {
        if (cpus[i])
            free_cpu(cpus[i]);
    }
    return -ENOMEM;
}

static void cleanup_system(void) {
    uint32_t i;
    
    system_running = false;
    
    /* Wake up all CPU threads */
    for (i = 0; i < nr_cpus; i++) {
        if (ipi_queues[i]) {
            pthread_mutex_lock(&ipi_queues[i]->lock);
            pthread_cond_broadcast(&ipi_queues[i]->wait);
            pthread_mutex_unlock(&ipi_queues[i]->lock);
        }
    }
    
    /* Wait for CPU threads to finish */
    for (i = 0; i < nr_cpus; i++) {
        if (cpus[i])
            pthread_join(cpus[i]->thread, NULL);
    }
    
    /* Free resources */
    for (i = 0; i < nr_cpus; i++) {
        if (cpu_caches[i])
            free_cpu_cache(cpu_caches[i]);
        if (ipi_queues[i])
            free_ipi_queue(ipi_queues[i]);
        if (cpus[i])
            free_cpu(cpus[i]);
        
        cpu_caches[i] = NULL;
        ipi_queues[i] = NULL;
        cpus[i] = NULL;
    }
}

/* Example Usage and Testing */

static void test_func(void *data) {
    uint32_t cpu = *(uint32_t *)data;
    printf("Test function running on CPU%u\n", cpu);
}

static void run_smp_test(void) {
    uint32_t cpu_data[ARM_MAX_CPUS];
    int i, ret;
    
    printf("ARM SMP Simulation\n");
    printf("=================\n\n");
    
    /* Initialize system with 4 CPUs */
    ret = init_system(4);
    if (ret) {
        printf("Failed to initialize system: %d\n", ret);
        return;
    }
    
    printf("System initialized with %u CPUs\n\n", nr_cpus);
    
    /* Let CPUs start up */
    sleep(1);
    
    printf("Sending IPIs to all CPUs:\n");
    
    /* Send some test IPIs */
    for (i = 0; i < nr_cpus; i++) {
        cpu_data[i] = i;
        
        /* Send function call IPI */
        ret = send_ipi_msg(0, i, IPI_CALL_FUNC,
                          &cpu_data[i], test_func);
        if (ret)
            printf("Failed to send IPI to CPU%d: %d\n",
                   i, ret);
    }
    
    /* Let IPIs process */
    sleep(1);
    
    printf("\nStopping CPUs:\n");
    
    /* Stop all CPUs */
    for (i = 1; i < nr_cpus; i++) {
        ret = send_ipi_msg(0, i, IPI_CPU_STOP, NULL, NULL);
        if (ret)
            printf("Failed to stop CPU%d: %d\n", i, ret);
    }
    
    /* Let CPUs stop */
    sleep(1);
    
    /* Cleanup */
    cleanup_system();
}

int main(void) {
    run_smp_test();
    return 0;
}
