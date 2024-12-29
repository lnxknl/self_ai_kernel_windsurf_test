/*
 * ARM Signal Handling Simulation
 * 
 * This program simulates the ARM signal handling subsystem,
 * including signal delivery, handler installation, and stack management.
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
#define ARM_MAX_SIGNALS    64
#define ARM_MAX_TASKS      128
#define ARM_MAX_HANDLERS   32
#define ARM_SIGFRAME_SIZE  1024
#define ARM_STACK_SIZE     8192
#define ARM_MAX_NAME_LEN   32

/* Signal Numbers (subset of standard UNIX signals) */
#define ARM_SIGHUP         1
#define ARM_SIGINT         2
#define ARM_SIGQUIT        3
#define ARM_SIGILL         4
#define ARM_SIGTRAP        5
#define ARM_SIGABRT        6
#define ARM_SIGBUS         7
#define ARM_SIGFPE         8
#define ARM_SIGKILL        9
#define ARM_SIGUSR1        10
#define ARM_SIGSEGV        11
#define ARM_SIGUSR2        12
#define ARM_SIGPIPE        13
#define ARM_SIGALRM        14
#define ARM_SIGTERM        15

/* Signal Actions */
#define ARM_SIG_DFL        0       /* Default action */
#define ARM_SIG_IGN        1       /* Ignore signal */
#define ARM_SIG_ERR        2       /* Error return */

/* Signal Flags */
#define ARM_SA_NOCLDSTOP   0x00000001
#define ARM_SA_NOCLDWAIT   0x00000002
#define ARM_SA_SIGINFO     0x00000004
#define ARM_SA_RESTART     0x10000000
#define ARM_SA_NODEFER     0x40000000
#define ARM_SA_RESETHAND   0x80000000

/* Register Set */
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
};

/* Signal Information */
struct siginfo {
    int      si_signo;     /* Signal number */
    int      si_code;      /* Signal code */
    int      si_errno;     /* Error number */
    pid_t    si_pid;       /* Sending process ID */
    uid_t    si_uid;       /* Real user ID of sender */
    void    *si_addr;      /* Address of faulting instruction */
    int      si_status;    /* Exit value or signal */
    long     si_band;      /* Band event */
    union {
        struct {
            void *_call_addr;   /* System call address */
            int   _syscall;     /* System call number */
            unsigned int _arch; /* AUDIT_ARCH_* */
        } _sigsys;
    } _sifields;
};

/* Signal Action Structure */
struct sigaction {
    void     (*sa_handler)(int);
    void     (*sa_sigaction)(int, struct siginfo *, void *);
    uint32_t   sa_flags;
    void     (*sa_restorer)(void);
    uint64_t   sa_mask;
};

/* Signal Frame */
struct sigframe {
    struct arm_regs regs;
    uint32_t oldmask;
    uint32_t tramp[2];
};

/* RT Signal Frame */
struct rt_sigframe {
    struct siginfo *info;
    void *uc;
    struct sigframe frame;
};

/* Task Structure */
struct task {
    pid_t     pid;
    char      name[ARM_MAX_NAME_LEN];
    uint32_t  state;
    struct arm_regs regs;
    void     *stack;
    uint32_t  stack_size;
    struct sigaction sighand[ARM_MAX_SIGNALS];
    uint64_t  pending;
    uint64_t  blocked;
    struct task *next;
    pthread_mutex_t lock;
};

/* Signal Statistics */
struct sig_stats {
    uint64_t generated;    /* Signals generated */
    uint64_t delivered;    /* Signals delivered */
    uint64_t ignored;      /* Signals ignored */
    uint64_t errors;       /* Delivery errors */
};

/* Global Variables */
static struct task *tasks[ARM_MAX_TASKS];
static struct sig_stats stats[ARM_MAX_SIGNALS];
static uint32_t nr_tasks = 0;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool system_running = false;

/* Function Prototypes */
static struct task *task_alloc(pid_t pid, const char *name);
static void task_free(struct task *task);
static int install_sighandler(struct task *task, int signum,
                            const struct sigaction *act);
static int send_signal(struct task *task, int signum,
                      struct siginfo *info);
static void handle_signal(struct task *task, int signum,
                        struct siginfo *info);

/* Task Management */

static struct task *task_alloc(pid_t pid, const char *name) {
    struct task *task;
    int i;
    
    task = calloc(1, sizeof(*task));
    if (!task)
        return NULL;
    
    task->pid = pid;
    strncpy(task->name, name, ARM_MAX_NAME_LEN - 1);
    task->stack_size = ARM_STACK_SIZE;
    task->stack = calloc(1, task->stack_size);
    if (!task->stack) {
        free(task);
        return NULL;
    }
    
    /* Initialize signal handlers to default */
    for (i = 0; i < ARM_MAX_SIGNALS; i++) {
        task->sighand[i].sa_handler = (void *)ARM_SIG_DFL;
        task->sighand[i].sa_flags = 0;
        task->sighand[i].sa_mask = 0;
    }
    
    pthread_mutex_init(&task->lock, NULL);
    
    return task;
}

static void task_free(struct task *task) {
    if (!task)
        return;
    
    free(task->stack);
    pthread_mutex_destroy(&task->lock);
    free(task);
}

/* Signal Handler Management */

static int install_sighandler(struct task *task, int signum,
                            const struct sigaction *act) {
    if (!task || signum >= ARM_MAX_SIGNALS)
        return -EINVAL;
    
    if (signum == ARM_SIGKILL || signum == ARM_SIGSTOP)
        return -EINVAL;
    
    pthread_mutex_lock(&task->lock);
    
    memcpy(&task->sighand[signum], act, sizeof(*act));
    
    pthread_mutex_unlock(&task->lock);
    return 0;
}

/* Signal Frame Management */

static int setup_sigframe(struct task *task, int signum,
                         struct siginfo *info) {
    struct sigframe *frame;
    uint32_t sp;
    
    /* Align stack pointer */
    sp = (task->regs.r13 - sizeof(*frame)) & ~7;
    frame = (struct sigframe *)sp;
    
    /* Save current registers */
    memcpy(&frame->regs, &task->regs, sizeof(frame->regs));
    
    /* Save signal mask */
    frame->oldmask = task->blocked;
    
    /* Setup signal trampoline */
    frame->tramp[0] = 0xe3a07000;  /* mov r7, #0 */
    frame->tramp[1] = 0xef000000;  /* svc 0x0 */
    
    /* Update stack pointer */
    task->regs.r13 = sp;
    
    return 0;
}

static int setup_rt_sigframe(struct task *task, int signum,
                           struct siginfo *info) {
    struct rt_sigframe *frame;
    uint32_t sp;
    
    /* Align stack pointer */
    sp = (task->regs.r13 - sizeof(*frame)) & ~7;
    frame = (struct rt_sigframe *)sp;
    
    /* Setup signal info */
    frame->info = info;
    frame->uc = NULL;  /* No user context for simulation */
    
    /* Setup regular frame */
    return setup_sigframe(task, signum, info);
}

/* Signal Delivery */

static void deliver_signal(struct task *task, int signum,
                         struct siginfo *info) {
    struct sigaction *sa = &task->sighand[signum];
    
    if ((uint32_t)sa->sa_handler == ARM_SIG_IGN) {
        stats[signum].ignored++;
        return;
    }
    
    if ((uint32_t)sa->sa_handler == ARM_SIG_DFL) {
        /* Default action: terminate */
        printf("Task %s(%d) terminated by signal %d\n",
               task->name, task->pid, signum);
        return;
    }
    
    /* Setup signal frame */
    if (sa->sa_flags & ARM_SA_SIGINFO) {
        if (setup_rt_sigframe(task, signum, info) < 0) {
            stats[signum].errors++;
            return;
        }
    } else {
        if (setup_sigframe(task, signum, info) < 0) {
            stats[signum].errors++;
            return;
        }
    }
    
    /* Block signals if SA_NODEFER not set */
    if (!(sa->sa_flags & ARM_SA_NODEFER))
        task->blocked |= (1ULL << signum);
    
    /* Call handler */
    if (sa->sa_flags & ARM_SA_SIGINFO)
        sa->sa_sigaction(signum, info, NULL);
    else
        sa->sa_handler(signum);
    
    stats[signum].delivered++;
    
    /* Reset handler if SA_RESETHAND set */
    if (sa->sa_flags & ARM_SA_RESETHAND)
        sa->sa_handler = (void *)ARM_SIG_DFL;
}

static int send_signal(struct task *task, int signum,
                      struct siginfo *info) {
    if (!task || signum >= ARM_MAX_SIGNALS)
        return -EINVAL;
    
    if (signum == 0)
        return 0;
    
    pthread_mutex_lock(&task->lock);
    
    /* Check if signal is blocked */
    if (task->blocked & (1ULL << signum)) {
        task->pending |= (1ULL << signum);
        pthread_mutex_unlock(&task->lock);
        return 0;
    }
    
    stats[signum].generated++;
    
    /* Deliver signal */
    deliver_signal(task, signum, info);
    
    pthread_mutex_unlock(&task->lock);
    return 0;
}

/* Example Signal Handlers */

static void sig_handler(int signum) {
    printf("Received signal %d\n", signum);
}

static void sig_info_handler(int signum, struct siginfo *info,
                           void *context) {
    printf("Received signal %d from PID %d\n",
           signum, info->si_pid);
}

/* System Initialization */

static int init_system(void) {
    struct task *task;
    struct sigaction sa;
    
    /* Create initial task */
    task = task_alloc(1, "init");
    if (!task)
        return -ENOMEM;
    
    tasks[nr_tasks++] = task;
    
    /* Install some signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    install_sighandler(task, ARM_SIGUSR1, &sa);
    
    sa.sa_sigaction = sig_info_handler;
    sa.sa_flags = ARM_SA_SIGINFO;
    install_sighandler(task, ARM_SIGUSR2, &sa);
    
    system_running = true;
    return 0;
}

static void cleanup_system(void) {
    uint32_t i;
    
    system_running = false;
    
    for (i = 0; i < nr_tasks; i++) {
        if (tasks[i])
            task_free(tasks[i]);
        tasks[i] = NULL;
    }
    
    nr_tasks = 0;
}

/* Example Usage and Testing */

static void print_sig_stats(void) {
    uint32_t i;
    
    printf("\nSignal Statistics:\n");
    for (i = 1; i < ARM_MAX_SIGNALS; i++) {
        if (stats[i].generated > 0) {
            printf("Signal %d:\n", i);
            printf("  Generated: %lu\n", stats[i].generated);
            printf("  Delivered: %lu\n", stats[i].delivered);
            printf("  Ignored:   %lu\n", stats[i].ignored);
            printf("  Errors:    %lu\n", stats[i].errors);
            printf("\n");
        }
    }
}

static void run_signal_test(void) {
    struct siginfo info;
    int ret;
    
    printf("ARM Signal Handling Simulation\n");
    printf("=============================\n\n");
    
    ret = init_system();
    if (ret) {
        printf("Failed to initialize system: %d\n", ret);
        return;
    }
    
    printf("System initialized\n\n");
    
    printf("Sending signals:\n");
    
    /* Send SIGUSR1 */
    memset(&info, 0, sizeof(info));
    info.si_signo = ARM_SIGUSR1;
    info.si_code = 0;
    ret = send_signal(tasks[0], ARM_SIGUSR1, &info);
    if (ret)
        printf("Failed to send SIGUSR1: %d\n", ret);
    
    /* Send SIGUSR2 */
    info.si_signo = ARM_SIGUSR2;
    info.si_pid = 2;
    ret = send_signal(tasks[0], ARM_SIGUSR2, &info);
    if (ret)
        printf("Failed to send SIGUSR2: %d\n", ret);
    
    /* Send ignored signal */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (void *)ARM_SIG_IGN;
    install_sighandler(tasks[0], ARM_SIGPIPE, &sa);
    
    info.si_signo = ARM_SIGPIPE;
    ret = send_signal(tasks[0], ARM_SIGPIPE, &info);
    if (ret)
        printf("Failed to send SIGPIPE: %d\n", ret);
    
    /* Print statistics */
    print_sig_stats();
    
    /* Cleanup */
    cleanup_system();
}

int main(void) {
    run_signal_test();
    return 0;
}
