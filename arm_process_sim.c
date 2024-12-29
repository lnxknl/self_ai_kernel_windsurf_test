/*
 * ARM Process Management Simulation
 * 
 * This program simulates the ARM process management subsystem,
 * including process creation, context switching, and thread management.
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
#define ARM_MAX_TASKS       256
#define ARM_STACK_SIZE      8192
#define ARM_MAX_PRIORITY    99
#define ARM_DEF_PRIORITY    20
#define ARM_TIME_SLICE      100  /* milliseconds */
#define ARM_MAX_CPUS        4
#define ARM_MAX_NAME_LEN    16

/* Process States */
#define TASK_RUNNING        0
#define TASK_INTERRUPTIBLE  1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_STOPPED        4
#define TASK_TRACED        8
#define TASK_ZOMBIE        16
#define TASK_DEAD          32

/* CPU Modes */
#define ARM_MODE_USER       0x10
#define ARM_MODE_FIQ       0x11
#define ARM_MODE_IRQ       0x12
#define ARM_MODE_SVC       0x13
#define ARM_MODE_ABORT     0x17
#define ARM_MODE_UND       0x1B
#define ARM_MODE_SYSTEM    0x1F

/* CPU Flags */
#define ARM_FLAG_N         (1 << 31)  /* Negative */
#define ARM_FLAG_Z         (1 << 30)  /* Zero */
#define ARM_FLAG_C         (1 << 29)  /* Carry */
#define ARM_FLAG_V         (1 << 28)  /* Overflow */
#define ARM_FLAG_I         (1 << 7)   /* IRQ disable */
#define ARM_FLAG_F         (1 << 6)   /* FIQ disable */
#define ARM_FLAG_T         (1 << 5)   /* Thumb state */

/* ARM Register Set */
struct arm_regs {
    uint32_t r0;           /* General purpose registers */
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;          /* FP */
    uint32_t r12;          /* IP */
    uint32_t r13;          /* SP */
    uint32_t r14;          /* LR */
    uint32_t r15;          /* PC */
    uint32_t cpsr;         /* Current Program Status Register */
    uint32_t spsr;         /* Saved Program Status Register */
};

/* Thread Info Structure */
struct thread_info {
    struct arm_regs regs;  /* CPU registers */
    uint32_t cpu;          /* CPU number */
    uint32_t preempt_count;/* Preemption counter */
    uint32_t addr_limit;   /* Address space limit */
    void    *stack;        /* Kernel stack */
};

/* Task Structure */
struct task_struct {
    pid_t pid;             /* Process ID */
    pid_t tgid;           /* Thread group ID */
    char comm[ARM_MAX_NAME_LEN]; /* Command name */
    uint32_t state;        /* Task state */
    uint32_t flags;        /* Task flags */
    int32_t prio;          /* Priority */
    int32_t static_prio;   /* Static priority */
    uint64_t utime;        /* User time */
    uint64_t stime;        /* System time */
    struct thread_info *thread; /* Thread information */
    struct task_struct *parent; /* Parent process */
    struct list_head children;  /* Children processes */
    struct list_head sibling;   /* Sibling processes */
    void *mm;              /* Memory descriptor */
    struct task_struct *next; /* Next task in run queue */
    pthread_mutex_t lock;   /* Task lock */
};

/* Run Queue Structure */
struct rq {
    uint32_t nr_running;   /* Number of running tasks */
    uint64_t clock;        /* Schedule clock */
    struct task_struct *curr; /* Current task */
    struct task_struct *idle; /* Idle task */
    struct task_struct *tasks[ARM_MAX_PRIORITY]; /* Priority arrays */
    pthread_mutex_t lock;   /* Queue lock */
    pthread_cond_t wait;    /* Wait condition */
};

/* CPU Structure */
struct cpu_info {
    uint32_t cpu_id;       /* CPU ID */
    struct rq *rq;         /* Run queue */
    struct task_struct *idle_task; /* Idle task */
    bool online;           /* CPU online status */
    pthread_t thread;      /* CPU thread */
    pthread_mutex_t lock;  /* CPU lock */
};

/* List Management */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* Global Variables */
static struct cpu_info *cpus[ARM_MAX_CPUS];
static uint32_t nr_cpus = 1;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool system_running = false;

/* Function Prototypes */
static struct task_struct *alloc_task(void);
static void free_task(struct task_struct *task);
static struct thread_info *alloc_thread_info(void);
static void free_thread_info(struct thread_info *thread);
static struct rq *alloc_rq(void);
static void free_rq(struct rq *rq);
static int arm_switch_to(struct task_struct *prev,
                        struct task_struct *next);

/* List Operations */

static void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static void list_add(struct list_head *new,
                    struct list_head *head) {
    head->next->prev = new;
    new->next = head->next;
    new->prev = head;
    head->next = new;
}

static void list_del(struct list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = NULL;
    entry->prev = NULL;
}

/* Task Operations */

static struct task_struct *alloc_task(void) {
    struct task_struct *task;
    
    task = calloc(1, sizeof(*task));
    if (!task)
        return NULL;
    
    task->thread = alloc_thread_info();
    if (!task->thread) {
        free(task);
        return NULL;
    }
    
    INIT_LIST_HEAD(&task->children);
    INIT_LIST_HEAD(&task->sibling);
    pthread_mutex_init(&task->lock, NULL);
    
    task->state = TASK_RUNNING;
    task->prio = ARM_DEF_PRIORITY;
    task->static_prio = ARM_DEF_PRIORITY;
    
    return task;
}

static void free_task(struct task_struct *task) {
    if (!task)
        return;
    
    if (task->thread)
        free_thread_info(task->thread);
    
    pthread_mutex_destroy(&task->lock);
    free(task);
}

/* Thread Info Operations */

static struct thread_info *alloc_thread_info(void) {
    struct thread_info *thread;
    
    thread = calloc(1, sizeof(*thread));
    if (!thread)
        return NULL;
    
    thread->stack = calloc(1, ARM_STACK_SIZE);
    if (!thread->stack) {
        free(thread);
        return NULL;
    }
    
    /* Initialize registers */
    thread->regs.cpsr = ARM_MODE_USER;
    thread->addr_limit = 0xBF000000;
    
    return thread;
}

static void free_thread_info(struct thread_info *thread) {
    if (!thread)
        return;
    
    free(thread->stack);
    free(thread);
}

/* Run Queue Operations */

static struct rq *alloc_rq(void) {
    struct rq *rq;
    
    rq = calloc(1, sizeof(*rq));
    if (!rq)
        return NULL;
    
    pthread_mutex_init(&rq->lock, NULL);
    pthread_cond_init(&rq->wait, NULL);
    
    return rq;
}

static void free_rq(struct rq *rq) {
    if (!rq)
        return;
    
    pthread_mutex_destroy(&rq->lock);
    pthread_cond_destroy(&rq->wait);
    free(rq);
}

/* CPU Operations */

static struct cpu_info *alloc_cpu(uint32_t cpu_id) {
    struct cpu_info *cpu;
    
    cpu = calloc(1, sizeof(*cpu));
    if (!cpu)
        return NULL;
    
    cpu->cpu_id = cpu_id;
    cpu->rq = alloc_rq();
    if (!cpu->rq) {
        free(cpu);
        return NULL;
    }
    
    cpu->idle_task = alloc_task();
    if (!cpu->idle_task) {
        free_rq(cpu->rq);
        free(cpu);
        return NULL;
    }
    
    snprintf(cpu->idle_task->comm, ARM_MAX_NAME_LEN,
             "idle_%u", cpu_id);
    cpu->idle_task->pid = -1;
    cpu->idle_task->state = TASK_RUNNING;
    cpu->idle_task->prio = ARM_MAX_PRIORITY - 1;
    
    pthread_mutex_init(&cpu->lock, NULL);
    cpu->online = true;
    
    return cpu;
}

static void free_cpu(struct cpu_info *cpu) {
    if (!cpu)
        return;
    
    if (cpu->idle_task)
        free_task(cpu->idle_task);
    if (cpu->rq)
        free_rq(cpu->rq);
    
    pthread_mutex_destroy(&cpu->lock);
    free(cpu);
}

/* Context Switch */

static int arm_switch_to(struct task_struct *prev,
                        struct task_struct *next) {
    struct thread_info *thread;
    
    if (!prev || !next)
        return -EINVAL;
    
    thread = next->thread;
    if (!thread)
        return -EINVAL;
    
    /* Save current task's context */
    /* Restore next task's context */
    printf("Switching from task %s (pid=%d) to %s (pid=%d)\n",
           prev->comm, prev->pid, next->comm, next->pid);
    
    return 0;
}

/* Scheduler Operations */

static void schedule_task(struct rq *rq,
                         struct task_struct *task) {
    int prio = task->prio;
    
    if (prio >= ARM_MAX_PRIORITY)
        prio = ARM_MAX_PRIORITY - 1;
    
    task->next = rq->tasks[prio];
    rq->tasks[prio] = task;
    rq->nr_running++;
}

static struct task_struct *pick_next_task(struct rq *rq) {
    struct task_struct *next = NULL;
    int prio;
    
    for (prio = 0; prio < ARM_MAX_PRIORITY; prio++) {
        if (rq->tasks[prio]) {
            next = rq->tasks[prio];
            rq->tasks[prio] = next->next;
            next->next = NULL;
            rq->nr_running--;
            break;
        }
    }
    
    return next;
}

/* CPU Thread */

static void *cpu_thread(void *data) {
    struct cpu_info *cpu = data;
    struct rq *rq = cpu->rq;
    struct task_struct *prev, *next;
    
    while (system_running) {
        pthread_mutex_lock(&rq->lock);
        
        prev = rq->curr;
        if (!prev)
            prev = cpu->idle_task;
        
        /* Pick next task to run */
        next = pick_next_task(rq);
        if (!next)
            next = cpu->idle_task;
        
        if (prev != next) {
            rq->curr = next;
            arm_switch_to(prev, next);
            
            /* If prev is still runnable, requeue it */
            if (prev != cpu->idle_task &&
                prev->state == TASK_RUNNING) {
                schedule_task(rq, prev);
            }
        }
        
        pthread_mutex_unlock(&rq->lock);
        
        /* Simulate time slice */
        usleep(ARM_TIME_SLICE * 1000);
    }
    
    return NULL;
}

/* System Initialization */

static int init_system(void) {
    uint32_t i;
    
    for (i = 0; i < nr_cpus; i++) {
        cpus[i] = alloc_cpu(i);
        if (!cpus[i])
            goto cleanup;
    }
    
    system_running = true;
    
    /* Start CPU threads */
    for (i = 0; i < nr_cpus; i++) {
        pthread_create(&cpus[i]->thread, NULL,
                      cpu_thread, cpus[i]);
    }
    
    return 0;
    
cleanup:
    while (i--) {
        if (cpus[i])
            free_cpu(cpus[i]);
    }
    return -ENOMEM;
}

static void cleanup_system(void) {
    uint32_t i;
    
    system_running = false;
    
    /* Wait for CPU threads to finish */
    for (i = 0; i < nr_cpus; i++) {
        if (cpus[i] && cpus[i]->online)
            pthread_join(cpus[i]->thread, NULL);
    }
    
    /* Free resources */
    for (i = 0; i < nr_cpus; i++) {
        if (cpus[i])
            free_cpu(cpus[i]);
        cpus[i] = NULL;
    }
}

/* Example Usage and Testing */

static void run_process_test(void) {
    struct task_struct *tasks[5];
    int i, ret;
    
    printf("ARM Process Management Simulation\n");
    printf("================================\n\n");
    
    /* Initialize system */
    ret = init_system();
    if (ret) {
        printf("Failed to initialize system: %d\n", ret);
        return;
    }
    
    printf("Creating test tasks:\n");
    
    /* Create some test tasks */
    for (i = 0; i < 5; i++) {
        tasks[i] = alloc_task();
        if (!tasks[i]) {
            printf("Failed to allocate task %d\n", i);
            continue;
        }
        
        snprintf(tasks[i]->comm, ARM_MAX_NAME_LEN,
                "task_%d", i);
        tasks[i]->pid = i + 1;
        tasks[i]->prio = ARM_DEF_PRIORITY - (i % 3);
        
        /* Schedule task on CPU 0 */
        pthread_mutex_lock(&cpus[0]->rq->lock);
        schedule_task(cpus[0]->rq, tasks[i]);
        pthread_mutex_unlock(&cpus[0]->rq->lock);
        
        printf("Created task: %s (pid=%d, prio=%d)\n",
               tasks[i]->comm, tasks[i]->pid, tasks[i]->prio);
    }
    
    /* Let system run for a while */
    printf("\nRunning simulation...\n");
    sleep(2);
    
    /* Cleanup */
    cleanup_system();
    
    for (i = 0; i < 5; i++) {
        if (tasks[i])
            free_task(tasks[i]);
    }
}

int main(void) {
    run_process_test();
    return 0;
}
