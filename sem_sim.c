/*
 * System V Semaphore Simulation
 * 
 * This program simulates the System V semaphore IPC mechanism based on
 * the Linux kernel's implementation. It provides a user-space simulation
 * of semaphores with similar functionality to the kernel.
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
#include <limits.h>

/* Configuration Constants */
#define SEMMSL      32      /* Max semaphores per array */
#define SEMMNI      128     /* Max number of semaphore sets */
#define SEMMNS      (SEMMSL * SEMMNI)  /* Max number of semaphores */
#define SEMOPM      32      /* Max number of operations per semop */
#define SEMVMX      32767   /* Maximum value for semaphore */

/* IPC Command Definitions */
#define IPC_CREAT   01000   /* Create key if key does not exist */
#define IPC_EXCL    02000   /* Fail if key exists */
#define IPC_NOWAIT  04000   /* Return error on wait */
#define IPC_RMID    0       /* Remove identifier */
#define IPC_SET     1       /* Set options */
#define IPC_STAT    2       /* Get options */
#define IPC_INFO    3       /* See ipcs */
#define IPC_PRIVATE ((key_t)0)

/* Semaphore Commands */
#define GETPID      11      /* Get sempid */
#define GETVAL      12      /* Get semval */
#define GETALL      13      /* Get all semval's */
#define GETNCNT     14      /* Get semncnt */
#define GETZCNT     15      /* Get semzcnt */
#define SETVAL      16      /* Set semval */
#define SETALL      17      /* Set all semval's */

/* Permission Bits */
#define SEM_A       0200    /* Alter permission */
#define SEM_R       0400    /* Read permission */

/* Error Codes */
#define EEXIST      17      /* File exists */
#define ENOENT      2       /* No such file or directory */
#define EINVAL      22      /* Invalid argument */
#define EFBIG       27      /* File too large */
#define ENOSPC      28      /* No space left on device */
#define EIDRM       43      /* Identifier removed */
#define EAGAIN      11      /* Try again */

/* Data Structures */

struct sem {
    int semval;             /* Current value */
    pid_t sempid;           /* PID of last operation */
    unsigned short semncnt;  /* Number of processes waiting for increase */
    unsigned short semzcnt;  /* Number of processes waiting for zero */
    time_t sem_otime;       /* Last semop time */
    pthread_mutex_t lock;    /* Per-semaphore lock */
};

struct sem_array {
    int id;                 /* Array identifier */
    key_t key;             /* Key supplied to semget() */
    time_t sem_ctime;      /* Last change time */
    time_t sem_otime;      /* Last semop time */
    unsigned short sem_nsems; /* Number of semaphores in set */
    int sem_perm;          /* Permissions */
    bool valid;            /* Whether this array is allocated */
    struct sem sems[SEMMSL]; /* Array of semaphores */
    pthread_mutex_t lock;   /* Array lock */
};

struct sembuf {
    unsigned short sem_num;  /* Semaphore number */
    short sem_op;           /* Operation (negative, 0, or positive) */
    short sem_flg;          /* Operation flags (IPC_NOWAIT, SEM_UNDO) */
};

struct sem_queue {
    struct sem_queue *next;
    struct sem_queue *prev;
    struct sembuf *sops;    /* Pending operations */
    int nsops;              /* Number of operations */
    int status;             /* Status of operation */
    pid_t pid;              /* Process ID of waiting task */
    bool wakeup;            /* Should this queue be woken up? */
};

/* Global Variables */
static struct sem_array *sem_arrays[SEMMNI];
static pthread_mutex_t sem_arrays_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int sem_arrays_used = ATOMIC_VAR_INIT(0);

/* Helper Functions */

static void sem_queue_init(struct sem_queue *q) {
    q->next = q;
    q->prev = q;
}

static void sem_queue_add(struct sem_queue *entry, struct sem_queue *head) {
    struct sem_queue *next = head->next;
    entry->next = next;
    entry->prev = head;
    next->prev = entry;
    head->next = entry;
}

static void sem_queue_del(struct sem_queue *entry) {
    struct sem_queue *prev = entry->prev;
    struct sem_queue *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

static bool sem_queue_empty(struct sem_queue *head) {
    return head->next == head;
}

/* Semaphore Management Functions */

static struct sem_array *find_sem_array(int id) {
    if (id < 0 || id >= SEMMNI)
        return NULL;
    return sem_arrays[id];
}

static int new_sem_array(key_t key, int nsems, int semflg) {
    struct sem_array *sma;
    int id, i;

    if (nsems < 0 || nsems > SEMMSL)
        return -EINVAL;

    pthread_mutex_lock(&sem_arrays_lock);

    /* If key is not IPC_PRIVATE, check if key already exists */
    if (key != IPC_PRIVATE) {
        for (id = 0; id < SEMMNI; id++) {
            sma = sem_arrays[id];
            if (sma && sma->valid && sma->key == key) {
                if (semflg & IPC_CREAT && semflg & IPC_EXCL) {
                    pthread_mutex_unlock(&sem_arrays_lock);
                    return -EEXIST;
                }
                pthread_mutex_unlock(&sem_arrays_lock);
                return id;
            }
        }
    }

    /* Find first available slot */
    for (id = 0; id < SEMMNI; id++) {
        if (!sem_arrays[id])
            break;
    }

    if (id >= SEMMNI) {
        pthread_mutex_unlock(&sem_arrays_lock);
        return -ENOSPC;
    }

    /* Allocate and initialize new array */
    sma = calloc(1, sizeof(*sma));
    if (!sma) {
        pthread_mutex_unlock(&sem_arrays_lock);
        return -ENOMEM;
    }

    sma->id = id;
    sma->key = key;
    sma->sem_perm = semflg & 0777;
    sma->sem_ctime = time(NULL);
    sma->sem_nsems = nsems;
    sma->valid = true;
    pthread_mutex_init(&sma->lock, NULL);

    for (i = 0; i < nsems; i++) {
        sma->sems[i].semval = 0;
        sma->sems[i].sempid = 0;
        sma->sems[i].semncnt = 0;
        sma->sems[i].semzcnt = 0;
        pthread_mutex_init(&sma->sems[i].lock, NULL);
    }

    sem_arrays[id] = sma;
    atomic_fetch_add(&sem_arrays_used, 1);

    pthread_mutex_unlock(&sem_arrays_lock);
    return id;
}

/* System V Semaphore API Implementation */

int semget(key_t key, int nsems, int semflg) {
    struct sem_array *sma;
    int id;

    if (key == IPC_PRIVATE || (semflg & IPC_CREAT)) {
        return new_sem_array(key, nsems, semflg);
    }

    /* Look up existing array */
    pthread_mutex_lock(&sem_arrays_lock);
    for (id = 0; id < SEMMNI; id++) {
        sma = sem_arrays[id];
        if (sma && sma->valid && sma->key == key) {
            if (nsems > sma->sem_nsems) {
                pthread_mutex_unlock(&sem_arrays_lock);
                return -EINVAL;
            }
            pthread_mutex_unlock(&sem_arrays_lock);
            return id;
        }
    }
    pthread_mutex_unlock(&sem_arrays_lock);
    return -ENOENT;
}

static int perform_op(struct sem *sem, struct sembuf *sop) {
    int result = 0;

    pthread_mutex_lock(&sem->lock);

    if (sop->sem_op > 0) {
        /* Increment operation */
        if (sem->semval + sop->sem_op > SEMVMX) {
            result = -ERANGE;
            goto out;
        }
        sem->semval += sop->sem_op;
        sem->sempid = getpid();
    } else if (sop->sem_op < 0) {
        /* Decrement operation */
        if (sem->semval < -sop->sem_op) {
            if (sop->sem_flg & IPC_NOWAIT) {
                result = -EAGAIN;
                goto out;
            }
            sem->semncnt++;
            while (sem->semval < -sop->sem_op) {
                pthread_mutex_unlock(&sem->lock);
                usleep(1000);  /* Sleep for 1ms */
                pthread_mutex_lock(&sem->lock);
            }
            sem->semncnt--;
        }
        sem->semval += sop->sem_op;
        sem->sempid = getpid();
    } else {
        /* Wait for zero */
        if (sem->semval != 0) {
            if (sop->sem_flg & IPC_NOWAIT) {
                result = -EAGAIN;
                goto out;
            }
            sem->semzcnt++;
            while (sem->semval != 0) {
                pthread_mutex_unlock(&sem->lock);
                usleep(1000);  /* Sleep for 1ms */
                pthread_mutex_lock(&sem->lock);
            }
            sem->semzcnt--;
        }
    }

out:
    pthread_mutex_unlock(&sem->lock);
    return result;
}

int semop(int semid, struct sembuf *sops, size_t nsops) {
    struct sem_array *sma;
    int error;
    size_t i;

    if (nsops < 1 || nsops > SEMOPM)
        return -EINVAL;

    sma = find_sem_array(semid);
    if (!sma || !sma->valid)
        return -EINVAL;

    pthread_mutex_lock(&sma->lock);

    /* Check array permissions */
    if ((sma->sem_perm & 0222) == 0) {
        pthread_mutex_unlock(&sma->lock);
        return -EACCES;
    }

    /* Perform all operations */
    for (i = 0; i < nsops; i++) {
        if (sops[i].sem_num >= sma->sem_nsems) {
            pthread_mutex_unlock(&sma->lock);
            return -EFBIG;
        }

        error = perform_op(&sma->sems[sops[i].sem_num], &sops[i]);
        if (error < 0) {
            pthread_mutex_unlock(&sma->lock);
            return error;
        }
    }

    sma->sem_otime = time(NULL);
    pthread_mutex_unlock(&sma->lock);
    return 0;
}

int semctl(int semid, int semnum, int cmd, ...) {
    struct sem_array *sma;
    int error = 0;
    va_list arg;
    union semun {
        int val;
        struct semid_ds *buf;
        unsigned short *array;
    } args;

    va_start(arg, cmd);
    args = va_arg(arg, union semun);
    va_end(arg);

    sma = find_sem_array(semid);
    if (!sma || !sma->valid)
        return -EINVAL;

    switch (cmd) {
        case IPC_RMID:
            pthread_mutex_lock(&sem_arrays_lock);
            pthread_mutex_lock(&sma->lock);
            sma->valid = false;
            sem_arrays[semid] = NULL;
            atomic_fetch_sub(&sem_arrays_used, 1);
            pthread_mutex_unlock(&sma->lock);
            pthread_mutex_destroy(&sma->lock);
            for (int i = 0; i < sma->sem_nsems; i++)
                pthread_mutex_destroy(&sma->sems[i].lock);
            free(sma);
            pthread_mutex_unlock(&sem_arrays_lock);
            break;

        case GETVAL:
            if (semnum < 0 || semnum >= sma->sem_nsems)
                return -EINVAL;
            pthread_mutex_lock(&sma->sems[semnum].lock);
            error = sma->sems[semnum].semval;
            pthread_mutex_unlock(&sma->sems[semnum].lock);
            break;

        case SETVAL:
            if (semnum < 0 || semnum >= sma->sem_nsems)
                return -EINVAL;
            if (args.val < 0 || args.val > SEMVMX)
                return -ERANGE;
            pthread_mutex_lock(&sma->sems[semnum].lock);
            sma->sems[semnum].semval = args.val;
            sma->sems[semnum].sempid = getpid();
            pthread_mutex_unlock(&sma->sems[semnum].lock);
            sma->sem_ctime = time(NULL);
            break;

        case GETALL:
            pthread_mutex_lock(&sma->lock);
            for (int i = 0; i < sma->sem_nsems; i++) {
                pthread_mutex_lock(&sma->sems[i].lock);
                args.array[i] = sma->sems[i].semval;
                pthread_mutex_unlock(&sma->sems[i].lock);
            }
            pthread_mutex_unlock(&sma->lock);
            break;

        case SETALL:
            pthread_mutex_lock(&sma->lock);
            for (int i = 0; i < sma->sem_nsems; i++) {
                if (args.array[i] > SEMVMX) {
                    pthread_mutex_unlock(&sma->lock);
                    return -ERANGE;
                }
                pthread_mutex_lock(&sma->sems[i].lock);
                sma->sems[i].semval = args.array[i];
                sma->sems[i].sempid = getpid();
                pthread_mutex_unlock(&sma->sems[i].lock);
            }
            sma->sem_ctime = time(NULL);
            pthread_mutex_unlock(&sma->lock);
            break;

        case GETNCNT:
            if (semnum < 0 || semnum >= sma->sem_nsems)
                return -EINVAL;
            pthread_mutex_lock(&sma->sems[semnum].lock);
            error = sma->sems[semnum].semncnt;
            pthread_mutex_unlock(&sma->sems[semnum].lock);
            break;

        case GETZCNT:
            if (semnum < 0 || semnum >= sma->sem_nsems)
                return -EINVAL;
            pthread_mutex_lock(&sma->sems[semnum].lock);
            error = sma->sems[semnum].semzcnt;
            pthread_mutex_unlock(&sma->sems[semnum].lock);
            break;

        case GETPID:
            if (semnum < 0 || semnum >= sma->sem_nsems)
                return -EINVAL;
            pthread_mutex_lock(&sma->sems[semnum].lock);
            error = sma->sems[semnum].sempid;
            pthread_mutex_unlock(&sma->sems[semnum].lock);
            break;

        default:
            error = -EINVAL;
    }

    return error;
}

/* Demo Functions */

static void print_sem_info(int semid) {
    struct sem_array *sma = find_sem_array(semid);
    if (!sma || !sma->valid) {
        printf("Invalid semaphore ID: %d\n", semid);
        return;
    }

    printf("\nSemaphore Array Information (ID: %d)\n", semid);
    printf("Key: 0x%x\n", sma->key);
    printf("Number of semaphores: %d\n", sma->sem_nsems);
    printf("Permissions: %o\n", sma->sem_perm);
    printf("Creation time: %s", ctime(&sma->sem_ctime));
    printf("Last operation time: %s", ctime(&sma->sem_otime));

    printf("\nSemaphore Values:\n");
    for (int i = 0; i < sma->sem_nsems; i++) {
        pthread_mutex_lock(&sma->sems[i].lock);
        printf("Sem[%d]: value=%d, pid=%d, ncnt=%d, zcnt=%d\n",
               i, sma->sems[i].semval, sma->sems[i].sempid,
               sma->sems[i].semncnt, sma->sems[i].semzcnt);
        pthread_mutex_unlock(&sma->sems[i].lock);
    }
    printf("\n");
}

/* Main Function */

int main(void) {
    int semid;
    struct sembuf sops[2];
    union semun arg;
    unsigned short values[2] = {1, 1};

    printf("System V Semaphore Simulation\n");
    printf("=============================\n\n");

    /* Create a semaphore set with 2 semaphores */
    semid = semget(IPC_PRIVATE, 2, IPC_CREAT | 0666);
    if (semid < 0) {
        perror("semget");
        return 1;
    }

    printf("Created semaphore array with ID: %d\n", semid);

    /* Initialize semaphores */
    arg.array = values;
    if (semctl(semid, 0, SETALL, arg) < 0) {
        perror("semctl SETALL");
        return 1;
    }

    /* Print initial state */
    print_sem_info(semid);

    /* Demonstrate semaphore operations */
    printf("Performing semaphore operations...\n");

    /* Decrement first semaphore */
    sops[0].sem_num = 0;
    sops[0].sem_op = -1;
    sops[0].sem_flg = 0;

    printf("Waiting on first semaphore...\n");
    if (semop(semid, sops, 1) < 0) {
        perror("semop");
        return 1;
    }

    print_sem_info(semid);

    /* Increment first semaphore */
    sops[0].sem_op = 1;
    printf("Releasing first semaphore...\n");
    if (semop(semid, sops, 1) < 0) {
        perror("semop");
        return 1;
    }

    print_sem_info(semid);

    /* Wait for zero on second semaphore */
    sops[0].sem_num = 1;
    sops[0].sem_op = 0;
    printf("Waiting for second semaphore to become zero...\n");
    
    /* In another thread, we would decrease it to zero */
    sops[1].sem_num = 1;
    sops[1].sem_op = -1;
    if (semop(semid, &sops[1], 1) < 0) {
        perror("semop");
        return 1;
    }

    if (semop(semid, sops, 1) < 0) {
        perror("semop");
        return 1;
    }

    print_sem_info(semid);

    /* Clean up */
    printf("Removing semaphore array...\n");
    if (semctl(semid, 0, IPC_RMID) < 0) {
        perror("semctl IPC_RMID");
        return 1;
    }

    printf("Done.\n");
    return 0;
}
