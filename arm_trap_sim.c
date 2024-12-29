/*
 * ARM Trap Handling Simulation
 * 
 * This program simulates the ARM trap and exception handling subsystem,
 * including various types of exceptions, trap handlers, and debug support.
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
#define ARM_MAX_VECTORS     16
#define ARM_MAX_HANDLERS    32
#define ARM_MAX_STACK_FRAMES 64
#define ARM_STACK_SIZE      8192
#define ARM_MAX_NAME_LEN    32

/* Exception Types */
#define EXC_RESET          0
#define EXC_UNDEF          1
#define EXC_SWI            2
#define EXC_PREFETCH_ABORT 3
#define EXC_DATA_ABORT     4
#define EXC_IRQ            5
#define EXC_FIQ            6
#define EXC_MAX            7

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
    uint32_t spsr;         /* Saved Program Status Register */
};

/* Stack Frame */
struct stack_frame {
    uint32_t pc;           /* Program Counter */
    uint32_t lr;           /* Link Register */
    uint32_t sp;           /* Stack Pointer */
    uint32_t fp;           /* Frame Pointer */
};

/* Exception Frame */
struct exc_frame {
    struct arm_regs regs;  /* CPU registers */
    uint32_t vector;       /* Exception vector */
    uint32_t fault_addr;   /* Fault address */
    uint32_t fault_status; /* Fault status */
    char     comm[ARM_MAX_NAME_LEN]; /* Task name */
};

/* Exception Handler */
struct exc_handler {
    void (*handler)(struct exc_frame *frame);
    char  name[ARM_MAX_NAME_LEN];
    bool  active;
};

/* CPU Context */
struct cpu_context {
    uint32_t cpu_id;       /* CPU ID */
    uint32_t current_mode; /* Current CPU mode */
    struct arm_regs regs;  /* Register set */
    struct stack_frame call_stack[ARM_MAX_STACK_FRAMES];
    uint32_t stack_depth;  /* Current stack depth */
    void    *stack;        /* CPU stack */
    pthread_mutex_t lock;  /* CPU lock */
};

/* Exception Statistics */
struct exc_stats {
    uint64_t count;        /* Number of exceptions */
    uint64_t handled;      /* Successfully handled */
    uint64_t unhandled;    /* Unhandled exceptions */
    uint64_t cycles;       /* Total cycles spent */
};

/* Global Variables */
static struct cpu_context *cpus[ARM_MAX_CPUS];
static struct exc_handler handlers[EXC_MAX][ARM_MAX_HANDLERS];
static struct exc_stats stats[EXC_MAX];
static uint32_t nr_cpus = 0;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool system_running = false;

/* Function Prototypes */
static struct cpu_context *cpu_alloc(uint32_t cpu_id);
static void cpu_free(struct cpu_context *cpu);
static int register_handler(uint32_t vector,
                          void (*handler)(struct exc_frame *),
                          const char *name);
static void unregister_handler(uint32_t vector, const char *name);
static void handle_exception(struct cpu_context *cpu,
                           uint32_t vector,
                           uint32_t fault_addr,
                           uint32_t fault_status);

/* CPU Management */

static struct cpu_context *cpu_alloc(uint32_t cpu_id) {
    struct cpu_context *cpu;
    
    cpu = calloc(1, sizeof(*cpu));
    if (!cpu)
        return NULL;
    
    cpu->cpu_id = cpu_id;
    cpu->current_mode = ARM_MODE_SVC;
    cpu->stack = calloc(1, ARM_STACK_SIZE);
    if (!cpu->stack) {
        free(cpu);
        return NULL;
    }
    
    pthread_mutex_init(&cpu->lock, NULL);
    
    /* Initialize registers */
    cpu->regs.cpsr = ARM_MODE_SVC | ARM_FLAG_I | ARM_FLAG_F;
    cpu->regs.r13 = (uint32_t)cpu->stack + ARM_STACK_SIZE; /* SP */
    cpu->regs.r15 = 0x8000; /* PC */
    
    return cpu;
}

static void cpu_free(struct cpu_context *cpu) {
    if (!cpu)
        return;
    
    free(cpu->stack);
    pthread_mutex_destroy(&cpu->lock);
    free(cpu);
}

/* Stack Management */

static void push_frame(struct cpu_context *cpu,
                      uint32_t pc, uint32_t lr,
                      uint32_t sp, uint32_t fp) {
    if (cpu->stack_depth >= ARM_MAX_STACK_FRAMES)
        return;
    
    struct stack_frame *frame = &cpu->call_stack[cpu->stack_depth++];
    frame->pc = pc;
    frame->lr = lr;
    frame->sp = sp;
    frame->fp = fp;
}

static void pop_frame(struct cpu_context *cpu) {
    if (cpu->stack_depth > 0)
        cpu->stack_depth--;
}

/* Exception Handling */

static int register_handler(uint32_t vector,
                          void (*handler)(struct exc_frame *),
                          const char *name) {
    uint32_t i;
    
    if (vector >= EXC_MAX || !handler || !name)
        return -EINVAL;
    
    pthread_mutex_lock(&global_lock);
    
    /* Find free slot */
    for (i = 0; i < ARM_MAX_HANDLERS; i++) {
        if (!handlers[vector][i].active) {
            handlers[vector][i].handler = handler;
            strncpy(handlers[vector][i].name, name,
                   ARM_MAX_NAME_LEN - 1);
            handlers[vector][i].active = true;
            pthread_mutex_unlock(&global_lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&global_lock);
    return -ENOSPC;
}

static void unregister_handler(uint32_t vector, const char *name) {
    uint32_t i;
    
    if (vector >= EXC_MAX || !name)
        return;
    
    pthread_mutex_lock(&global_lock);
    
    for (i = 0; i < ARM_MAX_HANDLERS; i++) {
        if (handlers[vector][i].active &&
            strcmp(handlers[vector][i].name, name) == 0) {
            handlers[vector][i].active = false;
            break;
        }
    }
    
    pthread_mutex_unlock(&global_lock);
}

static void handle_exception(struct cpu_context *cpu,
                           uint32_t vector,
                           uint32_t fault_addr,
                           uint32_t fault_status) {
    struct exc_frame frame;
    uint32_t i;
    bool handled = false;
    
    if (vector >= EXC_MAX)
        return;
    
    /* Prepare exception frame */
    memcpy(&frame.regs, &cpu->regs, sizeof(frame.regs));
    frame.vector = vector;
    frame.fault_addr = fault_addr;
    frame.fault_status = fault_status;
    snprintf(frame.comm, ARM_MAX_NAME_LEN, "cpu%u", cpu->cpu_id);
    
    /* Save current context */
    push_frame(cpu, cpu->regs.r15, cpu->regs.r14,
              cpu->regs.r13, cpu->regs.r11);
    
    /* Update statistics */
    stats[vector].count++;
    
    /* Call handlers */
    pthread_mutex_lock(&global_lock);
    
    for (i = 0; i < ARM_MAX_HANDLERS; i++) {
        if (handlers[vector][i].active) {
            handlers[vector][i].handler(&frame);
            handled = true;
        }
    }
    
    pthread_mutex_unlock(&global_lock);
    
    if (handled)
        stats[vector].handled++;
    else
        stats[vector].unhandled++;
    
    /* Restore context */
    pop_frame(cpu);
}

/* Example Exception Handlers */

static void undef_handler(struct exc_frame *frame) {
    printf("Undefined instruction at PC=0x%08x\n", frame->regs.r15);
}

static void swi_handler(struct exc_frame *frame) {
    printf("Software interrupt: r0=0x%08x\n", frame->regs.r0);
}

static void prefetch_abort_handler(struct exc_frame *frame) {
    printf("Prefetch abort at addr=0x%08x, status=0x%08x\n",
           frame->fault_addr, frame->fault_status);
}

static void data_abort_handler(struct exc_frame *frame) {
    printf("Data abort at addr=0x%08x, status=0x%08x\n",
           frame->fault_addr, frame->fault_status);
}

/* System Initialization */

static int init_system(uint32_t num_cpus) {
    uint32_t i;
    
    if (num_cpus > ARM_MAX_CPUS)
        num_cpus = ARM_MAX_CPUS;
    
    nr_cpus = num_cpus;
    
    /* Allocate CPUs */
    for (i = 0; i < nr_cpus; i++) {
        cpus[i] = cpu_alloc(i);
        if (!cpus[i])
            goto cleanup;
    }
    
    /* Register default handlers */
    register_handler(EXC_UNDEF, undef_handler, "undef");
    register_handler(EXC_SWI, swi_handler, "swi");
    register_handler(EXC_PREFETCH_ABORT,
                    prefetch_abort_handler, "prefetch_abort");
    register_handler(EXC_DATA_ABORT,
                    data_abort_handler, "data_abort");
    
    system_running = true;
    return 0;
    
cleanup:
    while (i--)
        cpu_free(cpus[i]);
    return -ENOMEM;
}

static void cleanup_system(void) {
    uint32_t i;
    
    system_running = false;
    
    /* Free CPUs */
    for (i = 0; i < nr_cpus; i++) {
        if (cpus[i])
            cpu_free(cpus[i]);
        cpus[i] = NULL;
    }
}

/* Example Usage and Testing */

static void print_exc_stats(void) {
    const char *exc_names[] = {
        "Reset", "Undefined", "SWI",
        "Prefetch Abort", "Data Abort",
        "IRQ", "FIQ"
    };
    uint32_t i;
    
    printf("\nException Statistics:\n");
    for (i = 0; i < EXC_MAX; i++) {
        if (stats[i].count > 0) {
            printf("%s:\n", exc_names[i]);
            printf("  Total:     %lu\n", stats[i].count);
            printf("  Handled:   %lu\n", stats[i].handled);
            printf("  Unhandled: %lu\n", stats[i].unhandled);
            printf("  Cycles:    %lu\n", stats[i].cycles);
            printf("\n");
        }
    }
}

static void run_trap_test(void) {
    int ret;
    
    printf("ARM Trap Handling Simulation\n");
    printf("===========================\n\n");
    
    /* Initialize system with 2 CPUs */
    ret = init_system(2);
    if (ret) {
        printf("Failed to initialize system: %d\n", ret);
        return;
    }
    
    printf("System initialized with %u CPUs\n\n", nr_cpus);
    
    printf("Simulating exceptions:\n");
    
    /* Simulate undefined instruction */
    handle_exception(cpus[0], EXC_UNDEF, 0, 0);
    
    /* Simulate software interrupt */
    cpus[0]->regs.r0 = 0x123;  /* SWI number */
    handle_exception(cpus[0], EXC_SWI, 0, 0);
    
    /* Simulate prefetch abort */
    handle_exception(cpus[0], EXC_PREFETCH_ABORT,
                    0x1000, 0x05);
    
    /* Simulate data abort */
    handle_exception(cpus[1], EXC_DATA_ABORT,
                    0x2000, 0x0F);
    
    /* Print statistics */
    print_exc_stats();
    
    /* Cleanup */
    cleanup_system();
}

int main(void) {
    run_trap_test();
    return 0;
}
