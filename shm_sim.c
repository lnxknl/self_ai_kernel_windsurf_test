/*
 * System V Shared Memory Simulation
 * 
 * This program simulates the System V shared memory IPC mechanism based on
 * the Linux kernel's implementation. It provides a user-space simulation
 * of shared memory segments with similar functionality to the kernel.
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

/* Configuration Constants */
#define SHMMAX      0x2000000           /* max shared seg size (bytes) */
#define SHMMIN      1                   /* min shared seg size (bytes) */
#define SHMMNI      4096                /* max number of segments */
#define SHMALL      (SHMMAX*SHMMNI)    /* max shared memory system wide (bytes) */

/* IPC Command Definitions */
#define IPC_CREAT   01000   /* Create key if key does not exist */
#define IPC_EXCL    02000   /* Fail if key exists */
#define IPC_NOWAIT  04000   /* Return error on wait */
#define IPC_RMID    0       /* Remove identifier */
#define IPC_SET     1       /* Set options */
#define IPC_STAT    2       /* Get options */
#define IPC_INFO    3       /* See ipcs */
#define IPC_PRIVATE ((key_t)0)

/* Permission Bits */
#define SHM_R       0400    /* Read permission */
#define SHM_W       0200    /* Write permission */

/* Special Operation Flags */
#define SHM_RDONLY  010000  /* Attach read-only (else read-write) */
#define SHM_RND     020000  /* Round attach address to SHMLBA */
#define SHM_REMAP   040000  /* Take-over region on attach */
#define SHM_EXEC    0100000 /* Execution permission */

/* Error Codes */
#define EEXIST      17      /* File exists */
#define ENOENT      2       /* No such file or directory */
#define EINVAL      22      /* Invalid argument */
#define ENOMEM      12      /* Out of memory */
#define EACCES      13      /* Permission denied */

/* Data Structures */

struct shm_perm {
    key_t key;              /* Key supplied to shmget() */
    uid_t uid;              /* Owner's user ID */
    gid_t gid;              /* Owner's group ID */
    uid_t cuid;             /* Creator's user ID */
    gid_t cgid;             /* Creator's group ID */
    unsigned short mode;     /* Permissions + SHM_DEST|SHM_LOCKED */
    unsigned short seq;      /* Sequence number */
};

struct shmid_ds {
    struct shm_perm shm_perm;   /* Operation permission struct */
    size_t shm_segsz;           /* Size of segment in bytes */
    time_t shm_atime;           /* Last attach time */
    time_t shm_dtime;           /* Last detach time */
    time_t shm_ctime;           /* Last change time */
    pid_t shm_cpid;             /* PID of creator */
    pid_t shm_lpid;             /* PID of last shmat/shmdt */
    unsigned long shm_nattch;    /* Number of current attaches */
    void *shm_memory;           /* Pointer to shared memory */
    bool valid;                 /* Whether this segment is allocated */
    pthread_mutex_t lock;       /* Segment lock */
};

struct shm_info {
    int used_ids;               /* Number of allocated segments */
    unsigned long shm_tot;      /* Total shared memory in bytes */
    unsigned long shm_rss;      /* Total resident shared memory */
    unsigned long shm_swp;      /* Total swapped shared memory */
    unsigned long swap_attempts;/* Attempts to swap */
    unsigned long swap_successes;/* Successful swaps */
};

/* Global Variables */
static struct shmid_ds *segments[SHMMNI];
static pthread_mutex_t segments_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int segments_used = ATOMIC_VAR_INIT(0);
static atomic_long total_memory_used = ATOMIC_VAR_INIT(0);

/* Helper Functions */

static struct shmid_ds *find_segment(int shmid) {
    if (shmid < 0 || shmid >= SHMMNI)
        return NULL;
    return segments[shmid];
}

static int check_permissions(struct shmid_ds *shp, int flag) {
    /* In this simulation, we'll do basic permission checking */
    if ((flag & IPC_W) && !(shp->shm_perm.mode & SHM_W))
        return -EACCES;
    if ((flag & IPC_R) && !(shp->shm_perm.mode & SHM_R))
        return -EACCES;
    return 0;
}

/* System V Shared Memory API Implementation */

int shmget(key_t key, size_t size, int shmflg) {
    struct shmid_ds *shp;
    int id, error;

    /* Validate size */
    if (size < SHMMIN || size > SHMMAX)
        return -EINVAL;

    /* Round up to page size */
    size = (size + 4095) & ~4095;

    pthread_mutex_lock(&segments_lock);

    /* Check if key exists */
    if (key != IPC_PRIVATE) {
        for (id = 0; id < SHMMNI; id++) {
            shp = segments[id];
            if (shp && shp->valid && shp->shm_perm.key == key) {
                if (shmflg & IPC_CREAT && shmflg & IPC_EXCL) {
                    error = -EEXIST;
                    goto out_unlock;
                }
                if (shp->shm_segsz < size) {
                    error = -EINVAL;
                    goto out_unlock;
                }
                if ((error = check_permissions(shp, shmflg)) != 0)
                    goto out_unlock;
                pthread_mutex_unlock(&segments_lock);
                return id;
            }
        }
    }

    /* Find free slot */
    for (id = 0; id < SHMMNI; id++) {
        if (!segments[id])
            break;
    }

    if (id >= SHMMNI) {
        error = -ENOSPC;
        goto out_unlock;
    }

    /* Check system-wide limits */
    if (atomic_load(&total_memory_used) + size > SHMALL) {
        error = -ENOMEM;
        goto out_unlock;
    }

    /* Allocate and initialize new segment */
    shp = calloc(1, sizeof(*shp));
    if (!shp) {
        error = -ENOMEM;
        goto out_unlock;
    }

    /* Allocate actual shared memory */
    shp->shm_memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shp->shm_memory == MAP_FAILED) {
        free(shp);
        error = -ENOMEM;
        goto out_unlock;
    }

    /* Initialize segment */
    shp->shm_perm.key = key;
    shp->shm_perm.mode = shmflg & 0777;
    shp->shm_perm.cuid = shp->shm_perm.uid = getuid();
    shp->shm_perm.cgid = shp->shm_perm.gid = getgid();
    shp->shm_segsz = size;
    shp->shm_cpid = getpid();
    shp->shm_lpid = 0;
    shp->shm_nattch = 0;
    shp->shm_ctime = time(NULL);
    shp->valid = true;
    pthread_mutex_init(&shp->lock, NULL);

    segments[id] = shp;
    atomic_fetch_add(&segments_used, 1);
    atomic_fetch_add(&total_memory_used, size);

    pthread_mutex_unlock(&segments_lock);
    return id;

out_unlock:
    pthread_mutex_unlock(&segments_lock);
    return error;
}

void *shmat(int shmid, const void *shmaddr, int shmflg) {
    struct shmid_ds *shp;
    void *addr;

    shp = find_segment(shmid);
    if (!shp || !shp->valid)
        return (void *)-1;

    pthread_mutex_lock(&shp->lock);

    /* Check permissions */
    if ((error = check_permissions(shp, 
        (shmflg & SHM_RDONLY) ? IPC_R : (IPC_R | IPC_W))) != 0) {
        pthread_mutex_unlock(&shp->lock);
        return (void *)-1;
    }

    /* Handle specific address request */
    if (shmaddr) {
        if (shmflg & SHM_RND)
            addr = (void *)((unsigned long)shmaddr & ~(SHMLBA-1));
        else
            addr = (void *)shmaddr;

        /* Try to map at specific address */
        addr = mmap(addr, shp->shm_segsz,
                   (shmflg & SHM_RDONLY) ? PROT_READ : (PROT_READ | PROT_WRITE),
                   MAP_SHARED | MAP_FIXED, -1, 0);
    } else {
        /* Let system choose address */
        addr = mmap(NULL, shp->shm_segsz,
                   (shmflg & SHM_RDONLY) ? PROT_READ : (PROT_READ | PROT_WRITE),
                   MAP_SHARED, -1, 0);
    }

    if (addr == MAP_FAILED) {
        pthread_mutex_unlock(&shp->lock);
        return (void *)-1;
    }

    /* Copy shared memory content */
    memcpy(addr, shp->shm_memory, shp->shm_segsz);

    /* Update segment info */
    shp->shm_nattch++;
    shp->shm_atime = time(NULL);
    shp->shm_lpid = getpid();

    pthread_mutex_unlock(&shp->lock);
    return addr;
}

int shmdt(const void *shmaddr) {
    struct shmid_ds *shp;
    int id;

    /* Find segment containing this address */
    pthread_mutex_lock(&segments_lock);
    for (id = 0; id < SHMMNI; id++) {
        shp = segments[id];
        if (shp && shp->valid) {
            pthread_mutex_lock(&shp->lock);
            if ((char *)shp->shm_memory <= (char *)shmaddr &&
                (char *)shmaddr < (char *)shp->shm_memory + shp->shm_segsz) {
                /* Found it */
                shp->shm_nattch--;
                shp->shm_dtime = time(NULL);
                shp->shm_lpid = getpid();
                pthread_mutex_unlock(&shp->lock);
                pthread_mutex_unlock(&segments_lock);
                return 0;
            }
            pthread_mutex_unlock(&shp->lock);
        }
    }
    pthread_mutex_unlock(&segments_lock);
    return -EINVAL;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    struct shmid_ds *shp;
    int error = 0;

    shp = find_segment(shmid);
    if (!shp || !shp->valid)
        return -EINVAL;

    switch (cmd) {
        case IPC_STAT:
            pthread_mutex_lock(&shp->lock);
            memcpy(buf, shp, sizeof(*buf));
            pthread_mutex_unlock(&shp->lock);
            break;

        case IPC_SET:
            pthread_mutex_lock(&shp->lock);
            shp->shm_perm.uid = buf->shm_perm.uid;
            shp->shm_perm.gid = buf->shm_perm.gid;
            shp->shm_perm.mode = (shp->shm_perm.mode & ~0777) |
                                (buf->shm_perm.mode & 0777);
            shp->shm_ctime = time(NULL);
            pthread_mutex_unlock(&shp->lock);
            break;

        case IPC_RMID:
            pthread_mutex_lock(&segments_lock);
            pthread_mutex_lock(&shp->lock);
            
            /* Mark for deletion */
            shp->valid = false;
            
            /* If no attachments, destroy now */
            if (shp->shm_nattch == 0) {
                segments[shmid] = NULL;
                atomic_fetch_sub(&segments_used, 1);
                atomic_fetch_sub(&total_memory_used, shp->shm_segsz);
                munmap(shp->shm_memory, shp->shm_segsz);
                pthread_mutex_unlock(&shp->lock);
                pthread_mutex_destroy(&shp->lock);
                free(shp);
            } else {
                pthread_mutex_unlock(&shp->lock);
            }
            pthread_mutex_unlock(&segments_lock);
            break;

        default:
            error = -EINVAL;
    }

    return error;
}

/* Demo Functions */

static void print_shm_info(int shmid) {
    struct shmid_ds *shp = find_segment(shmid);
    if (!shp || !shp->valid) {
        printf("Invalid shared memory ID: %d\n", shmid);
        return;
    }

    printf("\nShared Memory Segment Information (ID: %d)\n", shmid);
    printf("Key: 0x%x\n", shp->shm_perm.key);
    printf("Owner UID: %d\n", shp->shm_perm.uid);
    printf("Owner GID: %d\n", shp->shm_perm.gid);
    printf("Creator UID: %d\n", shp->shm_perm.cuid);
    printf("Creator GID: %d\n", shp->shm_perm.cgid);
    printf("Permissions: %o\n", shp->shm_perm.mode);
    printf("Size: %zu bytes\n", shp->shm_segsz);
    printf("Number of attaches: %lu\n", shp->shm_nattch);
    printf("Creator PID: %d\n", shp->shm_cpid);
    printf("Last operation PID: %d\n", shp->shm_lpid);
    printf("Last attach time: %s", ctime(&shp->shm_atime));
    printf("Last detach time: %s", ctime(&shp->shm_dtime));
    printf("Last change time: %s", ctime(&shp->shm_ctime));
}

/* Main Function */

int main(void) {
    int shmid;
    void *addr1, *addr2;
    struct shmid_ds buf;
    const char *test_string = "Hello, Shared Memory!";
    char buffer[64];

    printf("System V Shared Memory Simulation\n");
    printf("=================================\n\n");

    /* Create shared memory segment */
    shmid = shmget(IPC_PRIVATE, 1024, IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget");
        return 1;
    }

    printf("Created shared memory segment with ID: %d\n", shmid);

    /* Attach segment for writing */
    addr1 = shmat(shmid, NULL, 0);
    if (addr1 == (void *)-1) {
        perror("shmat");
        return 1;
    }

    printf("First process attached at address: %p\n", addr1);
    print_shm_info(shmid);

    /* Write to shared memory */
    strcpy(addr1, test_string);
    printf("Wrote to shared memory: %s\n", test_string);

    /* Attach segment again (simulating another process) */
    addr2 = shmat(shmid, NULL, SHM_RDONLY);
    if (addr2 == (void *)-1) {
        perror("shmat");
        return 1;
    }

    printf("Second process attached at address: %p\n", addr2);
    print_shm_info(shmid);

    /* Read from shared memory */
    strncpy(buffer, addr2, sizeof(buffer));
    printf("Read from shared memory: %s\n", buffer);

    /* Detach segments */
    if (shmdt(addr1) < 0) {
        perror("shmdt");
        return 1;
    }
    printf("First process detached\n");

    if (shmdt(addr2) < 0) {
        perror("shmdt");
        return 1;
    }
    printf("Second process detached\n");

    print_shm_info(shmid);

    /* Remove segment */
    if (shmctl(shmid, IPC_RMID, NULL) < 0) {
        perror("shmctl");
        return 1;
    }
    printf("Shared memory segment removed\n");

    return 0;
}
