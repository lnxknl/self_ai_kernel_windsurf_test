/*
 * ARM TCM (Tightly Coupled Memory) Simulation
 * 
 * This program simulates the ARM TCM subsystem, including ITCM
 * (Instruction TCM) and DTCM (Data TCM) management.
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
#include <sys/mman.h>

/* Configuration Constants */
#define TCM_MAX_REGIONS     4
#define TCM_MIN_SIZE       4096    /* 4KB */
#define TCM_MAX_SIZE       1048576 /* 1MB */
#define TCM_ALIGN          4096    /* Page size alignment */
#define TCM_MAX_NAME_LEN   32
#define TCM_ACCESS_TIME    1       /* Simulated access time in ns */
#define TCM_CACHE_LINE     32      /* Cache line size */

/* TCM Region Types */
#define TCM_TYPE_ITCM      0       /* Instruction TCM */
#define TCM_TYPE_DTCM      1       /* Data TCM */
#define TCM_TYPE_BOTH      2       /* Both ITCM and DTCM */

/* TCM Access Flags */
#define TCM_READ           (1 << 0)
#define TCM_WRITE          (1 << 1)
#define TCM_EXEC           (1 << 2)
#define TCM_SECURE         (1 << 3)
#define TCM_CACHED         (1 << 4)
#define TCM_BUFFERED      (1 << 5)

/* TCM States */
#define TCM_STATE_OFF      0
#define TCM_STATE_ON       1
#define TCM_STATE_ERROR    2

/* Error Codes */
#define TCM_SUCCESS        0
#define TCM_ERR_NOMEM     -1
#define TCM_ERR_INVAL     -2
#define TCM_ERR_BUSY      -3
#define TCM_ERR_ACCESS    -4
#define TCM_ERR_ALIGN     -5

/* Statistics Counters */
struct tcm_stats {
    uint64_t reads;        /* Number of read accesses */
    uint64_t writes;       /* Number of write accesses */
    uint64_t hits;         /* Cache hits */
    uint64_t misses;       /* Cache misses */
    uint64_t errors;       /* Access errors */
    uint64_t cycles;       /* Total cycles */
};

/* TCM Region Structure */
struct tcm_region {
    char     name[TCM_MAX_NAME_LEN]; /* Region name */
    uint32_t type;         /* Region type */
    uint32_t flags;        /* Access flags */
    uint32_t state;        /* Region state */
    uint64_t base;         /* Base address */
    uint64_t size;         /* Region size */
    void    *mem;          /* Memory pointer */
    struct tcm_stats stats;/* Access statistics */
    pthread_mutex_t lock;  /* Region lock */
};

/* TCM Controller Structure */
struct tcm_controller {
    uint32_t nr_regions;   /* Number of regions */
    struct tcm_region *regions[TCM_MAX_REGIONS]; /* TCM regions */
    pthread_mutex_t lock;  /* Controller lock */
    bool     initialized;  /* Initialization flag */
};

/* Global Variables */
static struct tcm_controller tcm_ctrl;

/* Function Prototypes */
static struct tcm_region *tcm_alloc_region(const char *name,
                                         uint32_t type,
                                         uint32_t flags,
                                         uint64_t size);
static void tcm_free_region(struct tcm_region *region);
static int tcm_map_region(struct tcm_region *region,
                         uint64_t base);
static void tcm_unmap_region(struct tcm_region *region);
static int tcm_access(struct tcm_region *region,
                     uint64_t addr, void *buf,
                     size_t len, uint32_t flags);

/* TCM Region Management */

static struct tcm_region *tcm_alloc_region(const char *name,
                                         uint32_t type,
                                         uint32_t flags,
                                         uint64_t size) {
    struct tcm_region *region;
    
    /* Validate parameters */
    if (!name || size < TCM_MIN_SIZE || size > TCM_MAX_SIZE)
        return NULL;
    
    /* Align size to page boundary */
    size = (size + TCM_ALIGN - 1) & ~(TCM_ALIGN - 1);
    
    region = calloc(1, sizeof(*region));
    if (!region)
        return NULL;
    
    strncpy(region->name, name, TCM_MAX_NAME_LEN - 1);
    region->type = type;
    region->flags = flags;
    region->size = size;
    region->state = TCM_STATE_OFF;
    
    pthread_mutex_init(&region->lock, NULL);
    
    /* Allocate memory for the region */
    region->mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region->mem == MAP_FAILED) {
        pthread_mutex_destroy(&region->lock);
        free(region);
        return NULL;
    }
    
    return region;
}

static void tcm_free_region(struct tcm_region *region) {
    if (!region)
        return;
    
    if (region->mem)
        munmap(region->mem, region->size);
    
    pthread_mutex_destroy(&region->lock);
    free(region);
}

static int tcm_map_region(struct tcm_region *region,
                         uint64_t base) {
    if (!region)
        return TCM_ERR_INVAL;
    
    pthread_mutex_lock(&region->lock);
    
    if (region->state != TCM_STATE_OFF) {
        pthread_mutex_unlock(&region->lock);
        return TCM_ERR_BUSY;
    }
    
    /* Check alignment */
    if (base & (TCM_ALIGN - 1)) {
        pthread_mutex_unlock(&region->lock);
        return TCM_ERR_ALIGN;
    }
    
    region->base = base;
    region->state = TCM_STATE_ON;
    
    pthread_mutex_unlock(&region->lock);
    return TCM_SUCCESS;
}

static void tcm_unmap_region(struct tcm_region *region) {
    if (!region)
        return;
    
    pthread_mutex_lock(&region->lock);
    region->state = TCM_STATE_OFF;
    pthread_mutex_unlock(&region->lock);
}

/* TCM Access Functions */

static int tcm_access(struct tcm_region *region,
                     uint64_t addr, void *buf,
                     size_t len, uint32_t flags) {
    uint64_t offset;
    
    if (!region || !buf)
        return TCM_ERR_INVAL;
    
    pthread_mutex_lock(&region->lock);
    
    if (region->state != TCM_STATE_ON) {
        pthread_mutex_unlock(&region->lock);
        return TCM_ERR_ACCESS;
    }
    
    /* Check address range */
    if (addr < region->base ||
        addr + len > region->base + region->size) {
        region->stats.errors++;
        pthread_mutex_unlock(&region->lock);
        return TCM_ERR_ACCESS;
    }
    
    /* Check access permissions */
    if ((flags & TCM_READ) && !(region->flags & TCM_READ)) {
        region->stats.errors++;
        pthread_mutex_unlock(&region->lock);
        return TCM_ERR_ACCESS;
    }
    
    if ((flags & TCM_WRITE) && !(region->flags & TCM_WRITE)) {
        region->stats.errors++;
        pthread_mutex_unlock(&region->lock);
        return TCM_ERR_ACCESS;
    }
    
    offset = addr - region->base;
    
    /* Perform access */
    if (flags & TCM_READ) {
        memcpy(buf, region->mem + offset, len);
        region->stats.reads++;
    } else if (flags & TCM_WRITE) {
        memcpy(region->mem + offset, buf, len);
        region->stats.writes++;
    }
    
    /* Simulate access time */
    usleep(TCM_ACCESS_TIME);
    region->stats.cycles += TCM_ACCESS_TIME;
    
    pthread_mutex_unlock(&region->lock);
    return TCM_SUCCESS;
}

/* TCM Controller Functions */

static int tcm_init(void) {
    pthread_mutex_lock(&tcm_ctrl.lock);
    
    if (tcm_ctrl.initialized) {
        pthread_mutex_unlock(&tcm_ctrl.lock);
        return TCM_SUCCESS;
    }
    
    memset(&tcm_ctrl, 0, sizeof(tcm_ctrl));
    pthread_mutex_init(&tcm_ctrl.lock, NULL);
    tcm_ctrl.initialized = true;
    
    pthread_mutex_unlock(&tcm_ctrl.lock);
    return TCM_SUCCESS;
}

static void tcm_cleanup(void) {
    uint32_t i;
    
    pthread_mutex_lock(&tcm_ctrl.lock);
    
    for (i = 0; i < tcm_ctrl.nr_regions; i++) {
        if (tcm_ctrl.regions[i]) {
            tcm_free_region(tcm_ctrl.regions[i]);
            tcm_ctrl.regions[i] = NULL;
        }
    }
    
    tcm_ctrl.nr_regions = 0;
    tcm_ctrl.initialized = false;
    
    pthread_mutex_unlock(&tcm_ctrl.lock);
    pthread_mutex_destroy(&tcm_ctrl.lock);
}

static int tcm_add_region(const char *name, uint32_t type,
                         uint32_t flags, uint64_t size,
                         uint64_t base) {
    struct tcm_region *region;
    int ret;
    
    pthread_mutex_lock(&tcm_ctrl.lock);
    
    if (tcm_ctrl.nr_regions >= TCM_MAX_REGIONS) {
        pthread_mutex_unlock(&tcm_ctrl.lock);
        return TCM_ERR_NOMEM;
    }
    
    region = tcm_alloc_region(name, type, flags, size);
    if (!region) {
        pthread_mutex_unlock(&tcm_ctrl.lock);
        return TCM_ERR_NOMEM;
    }
    
    ret = tcm_map_region(region, base);
    if (ret) {
        tcm_free_region(region);
        pthread_mutex_unlock(&tcm_ctrl.lock);
        return ret;
    }
    
    tcm_ctrl.regions[tcm_ctrl.nr_regions++] = region;
    
    pthread_mutex_unlock(&tcm_ctrl.lock);
    return TCM_SUCCESS;
}

/* Example Usage and Testing */

static void print_region_stats(struct tcm_region *region) {
    printf("Region %s statistics:\n", region->name);
    printf("  Reads:  %lu\n", region->stats.reads);
    printf("  Writes: %lu\n", region->stats.writes);
    printf("  Hits:   %lu\n", region->stats.hits);
    printf("  Misses: %lu\n", region->stats.misses);
    printf("  Errors: %lu\n", region->stats.errors);
    printf("  Cycles: %lu\n", region->stats.cycles);
}

static void run_tcm_test(void) {
    uint8_t buffer[256];
    int i, ret;
    
    printf("ARM TCM Simulation\n");
    printf("=================\n\n");
    
    /* Initialize TCM controller */
    ret = tcm_init();
    if (ret) {
        printf("Failed to initialize TCM: %d\n", ret);
        return;
    }
    
    /* Add some TCM regions */
    printf("Adding TCM regions:\n");
    
    ret = tcm_add_region("itcm0", TCM_TYPE_ITCM,
                        TCM_READ | TCM_EXEC,
                        32768, 0x00000000);
    if (ret) {
        printf("Failed to add ITCM region: %d\n", ret);
        goto out;
    }
    printf("Added ITCM region at 0x00000000\n");
    
    ret = tcm_add_region("dtcm0", TCM_TYPE_DTCM,
                        TCM_READ | TCM_WRITE,
                        16384, 0x20000000);
    if (ret) {
        printf("Failed to add DTCM region: %d\n", ret);
        goto out;
    }
    printf("Added DTCM region at 0x20000000\n");
    
    /* Perform some test accesses */
    printf("\nPerforming test accesses:\n");
    
    /* Write to DTCM */
    for (i = 0; i < sizeof(buffer); i++)
        buffer[i] = i;
    
    ret = tcm_access(tcm_ctrl.regions[1], 0x20000000,
                     buffer, sizeof(buffer), TCM_WRITE);
    if (ret) {
        printf("Failed to write to DTCM: %d\n", ret);
        goto out;
    }
    printf("Wrote %zu bytes to DTCM\n", sizeof(buffer));
    
    /* Read from DTCM */
    memset(buffer, 0, sizeof(buffer));
    ret = tcm_access(tcm_ctrl.regions[1], 0x20000000,
                     buffer, sizeof(buffer), TCM_READ);
    if (ret) {
        printf("Failed to read from DTCM: %d\n", ret);
        goto out;
    }
    printf("Read %zu bytes from DTCM\n", sizeof(buffer));
    
    /* Try to write to ITCM (should fail) */
    ret = tcm_access(tcm_ctrl.regions[0], 0x00000000,
                     buffer, sizeof(buffer), TCM_WRITE);
    if (ret == TCM_ERR_ACCESS) {
        printf("Write to ITCM correctly rejected\n");
    } else {
        printf("Unexpected result for ITCM write: %d\n", ret);
    }
    
    /* Print statistics */
    printf("\nTCM Statistics:\n");
    for (i = 0; i < tcm_ctrl.nr_regions; i++) {
        print_region_stats(tcm_ctrl.regions[i]);
        printf("\n");
    }
    
out:
    tcm_cleanup();
}

int main(void) {
    run_tcm_test();
    return 0;
}
