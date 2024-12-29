/*
 * Memory Shrinker System Simulation
 * 
 * This program simulates a memory shrinker system similar to the Linux kernel's
 * shrinker mechanism. It demonstrates how different subsystems can register
 * shrinkers to reclaim memory under pressure.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/time.h>

/* Configuration Constants */
#define MAX_SHRINKERS        32
#define MAX_CACHE_TYPES      16
#define MAX_OBJECTS_PER_CACHE 1024
#define SHRINK_BATCH         128
#define DEFAULT_CACHE_SIZE   (4 * 1024)  // 4KB per cache object
#define MAX_CACHE_NAME       64
#define MAX_SHRINKER_NAME    64

/* Priority Levels */
#define DEF_PRIORITY         0
#define HIGH_PRIORITY        1
#define CRITICAL_PRIORITY    2

/* Error Codes */
#define SUCCESS              0
#define ERR_NO_MEMORY      -1
#define ERR_INVALID_PARAM  -2
#define ERR_NOT_FOUND      -3
#define ERR_FULL           -4

/* Shrinker Flags */
#define SHRINKER_ACTIVE     0x1
#define SHRINKER_ASYNC      0x2
#define SHRINKER_DEFAULT    0x4

/* Object States */
#define OBJ_FREE            0
#define OBJ_USED            1
#define OBJ_INACTIVE        2

/* Forward Declarations */
struct shrinker;
struct cache_type;
struct cache_object;
struct shrink_control;

/* Type Definitions */

// Cache object structure
struct cache_object {
    int id;                     // Object ID
    int state;                  // Object state
    time_t last_access;        // Last access time
    void *data;               // Actual object data
    size_t size;             // Object size
    struct cache_type *cache; // Parent cache
};

// Cache type structure
struct cache_type {
    char name[MAX_CACHE_NAME];
    int id;
    size_t object_size;
    int total_objects;
    int used_objects;
    struct cache_object *objects[MAX_OBJECTS_PER_CACHE];
    pthread_mutex_t lock;
    struct shrinker *shrinker;
    time_t last_shrink;
    unsigned long flags;
};

// Shrink control structure
struct shrink_control {
    int priority;              // Shrink priority
    unsigned long nr_to_scan;  // Number of objects to scan
    unsigned long nr_scanned;  // Number of objects scanned
    unsigned long total_scan;  // Total number to scan
    gfp_t gfp_mask;          // Allocation flags
};

// Shrinker callbacks
typedef unsigned long (*shrink_count_t)(struct shrinker *s, struct shrink_control *sc);
typedef unsigned long (*shrink_scan_t)(struct shrinker *s, struct shrink_control *sc);

// Shrinker structure
struct shrinker {
    int id;
    char name[MAX_SHRINKER_NAME];
    shrink_count_t count_objects;
    shrink_scan_t scan_objects;
    struct cache_type *cache;
    long batch;
    long min_objects;
    unsigned long flags;
    pthread_mutex_t lock;
    struct timespec last_run;
};

/* Global Variables */
static struct shrinker *shrinkers[MAX_SHRINKERS];
static int nr_shrinkers = 0;
static pthread_mutex_t shrinker_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cache_type *cache_types[MAX_CACHE_TYPES];
static int nr_cache_types = 0;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* Utility Functions */

// Get current timestamp in milliseconds
static long long current_timestamp(void) {
    struct timeval te;
    gettimeofday(&te, NULL);
    return te.tv_sec * 1000LL + te.tv_usec / 1000;
}

// Calculate time difference in milliseconds
static long long time_diff_ms(struct timespec *start, struct timespec *end) {
    return ((end->tv_sec - start->tv_sec) * 1000LL) +
           ((end->tv_nsec - start->tv_nsec) / 1000000LL);
}

/* Cache Management Functions */

// Initialize a cache object
static void init_cache_object(struct cache_object *obj, int id, struct cache_type *cache) {
    obj->id = id;
    obj->state = OBJ_FREE;
    obj->last_access = time(NULL);
    obj->size = cache->object_size;
    obj->cache = cache;
    obj->data = calloc(1, cache->object_size);
}

// Create a new cache type
static struct cache_type *create_cache_type(const char *name, size_t object_size) {
    pthread_mutex_lock(&cache_lock);
    
    if (nr_cache_types >= MAX_CACHE_TYPES) {
        pthread_mutex_unlock(&cache_lock);
        return NULL;
    }

    struct cache_type *cache = calloc(1, sizeof(struct cache_type));
    if (!cache) {
        pthread_mutex_unlock(&cache_lock);
        return NULL;
    }

    strncpy(cache->name, name, MAX_CACHE_NAME - 1);
    cache->id = nr_cache_types++;
    cache->object_size = object_size;
    cache->total_objects = 0;
    cache->used_objects = 0;
    pthread_mutex_init(&cache->lock, NULL);
    cache->last_shrink = time(NULL);

    // Initialize cache objects
    for (int i = 0; i < MAX_OBJECTS_PER_CACHE; i++) {
        cache->objects[i] = calloc(1, sizeof(struct cache_object));
        if (cache->objects[i]) {
            init_cache_object(cache->objects[i], i, cache);
            cache->total_objects++;
        }
    }

    cache_types[cache->id] = cache;
    pthread_mutex_unlock(&cache_lock);
    return cache;
}

// Allocate an object from cache
static struct cache_object *cache_alloc(struct cache_type *cache) {
    struct cache_object *obj = NULL;
    
    pthread_mutex_lock(&cache->lock);
    
    // Find a free object
    for (int i = 0; i < cache->total_objects; i++) {
        if (cache->objects[i]->state == OBJ_FREE) {
            obj = cache->objects[i];
            obj->state = OBJ_USED;
            obj->last_access = time(NULL);
            cache->used_objects++;
            break;
        }
    }
    
    pthread_mutex_unlock(&cache->lock);
    return obj;
}

// Free a cache object
static void cache_free(struct cache_object *obj) {
    if (!obj || !obj->cache)
        return;

    struct cache_type *cache = obj->cache;
    pthread_mutex_lock(&cache->lock);
    
    if (obj->state == OBJ_USED) {
        obj->state = OBJ_FREE;
        obj->last_access = time(NULL);
        cache->used_objects--;
    }
    
    pthread_mutex_unlock(&cache->lock);
}

/* Shrinker Implementation */

// Default count_objects callback
static unsigned long default_count_objects(struct shrinker *s, struct shrink_control *sc) {
    if (!s || !s->cache)
        return 0;

    struct cache_type *cache = s->cache;
    return cache->used_objects;
}

// Default scan_objects callback
static unsigned long default_scan_objects(struct shrinker *s, struct shrink_control *sc) {
    if (!s || !s->cache || !sc)
        return 0;

    struct cache_type *cache = s->cache;
    unsigned long freed = 0;
    time_t current_time = time(NULL);
    
    pthread_mutex_lock(&cache->lock);
    
    // Scan objects and free inactive ones
    for (int i = 0; i < cache->total_objects && sc->nr_scanned < sc->nr_to_scan; i++) {
        struct cache_object *obj = cache->objects[i];
        if (obj->state == OBJ_USED) {
            // Free objects that haven't been accessed recently
            if (current_time - obj->last_access > 60) { // 60 seconds threshold
                cache_free(obj);
                freed++;
            }
            sc->nr_scanned++;
        }
    }
    
    pthread_mutex_unlock(&cache->lock);
    return freed;
}

// Create a new shrinker
static struct shrinker *create_shrinker(const char *name, 
                                      shrink_count_t count_fn,
                                      shrink_scan_t scan_fn) {
    pthread_mutex_lock(&shrinker_lock);
    
    if (nr_shrinkers >= MAX_SHRINKERS) {
        pthread_mutex_unlock(&shrinker_lock);
        return NULL;
    }

    struct shrinker *s = calloc(1, sizeof(struct shrinker));
    if (!s) {
        pthread_mutex_unlock(&shrinker_lock);
        return NULL;
    }

    s->id = nr_shrinkers++;
    strncpy(s->name, name, MAX_SHRINKER_NAME - 1);
    s->count_objects = count_fn ? count_fn : default_count_objects;
    s->scan_objects = scan_fn ? scan_fn : default_scan_objects;
    s->batch = SHRINK_BATCH;
    s->flags = SHRINKER_DEFAULT;
    pthread_mutex_init(&s->lock, NULL);
    clock_gettime(CLOCK_MONOTONIC, &s->last_run);

    shrinkers[s->id] = s;
    pthread_mutex_unlock(&shrinker_lock);
    return s;
}

// Register a shrinker for a cache
static int register_cache_shrinker(struct cache_type *cache, const char *name) {
    struct shrinker *s = create_shrinker(name, NULL, NULL);
    if (!s)
        return ERR_NO_MEMORY;

    s->cache = cache;
    cache->shrinker = s;
    return SUCCESS;
}

// Do shrink operation
static unsigned long do_shrink(struct shrinker *shrinker, struct shrink_control *sc) {
    unsigned long nr_objects;
    unsigned long freed = 0;
    struct timespec current_time;
    
    if (!shrinker || !sc)
        return 0;

    pthread_mutex_lock(&shrinker->lock);
    
    // Check if enough time has passed since last run
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    long long elapsed = time_diff_ms(&shrinker->last_run, &current_time);
    if (elapsed < 1000) { // Minimum 1 second between runs
        pthread_mutex_unlock(&shrinker->lock);
        return 0;
    }

    // Count total objects
    nr_objects = shrinker->count_objects(shrinker, sc);
    if (nr_objects <= shrinker->min_objects) {
        pthread_mutex_unlock(&shrinker->lock);
        return 0;
    }

    // Calculate how many objects to scan
    sc->nr_to_scan = min(nr_objects, sc->total_scan);
    sc->nr_scanned = 0;

    // Perform the scan
    freed = shrinker->scan_objects(shrinker, sc);
    
    // Update last run time
    shrinker->last_run = current_time;
    
    pthread_mutex_unlock(&shrinker->lock);
    return freed;
}

/* Memory Pressure Simulation */

// Structure to hold memory pressure state
struct memory_pressure {
    int level;              // Current pressure level
    unsigned long threshold;// Memory threshold
    bool active;           // Whether simulation is active
    pthread_t thread;      // Pressure simulation thread
    pthread_mutex_t lock;  // Lock for pressure state
};

static struct memory_pressure pressure_sim = {
    .level = 0,
    .threshold = 80,  // 80% usage triggers pressure
    .active = false,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

// Calculate current memory pressure level
static int calculate_pressure_level(void) {
    int total_used = 0;
    int total_objects = 0;
    
    pthread_mutex_lock(&cache_lock);
    
    for (int i = 0; i < nr_cache_types; i++) {
        if (cache_types[i]) {
            total_used += cache_types[i]->used_objects;
            total_objects += cache_types[i]->total_objects;
        }
    }
    
    pthread_mutex_unlock(&cache_lock);
    
    if (total_objects == 0)
        return 0;

    int usage_percent = (total_used * 100) / total_objects;
    
    if (usage_percent > 90)
        return CRITICAL_PRIORITY;
    else if (usage_percent > 80)
        return HIGH_PRIORITY;
    else
        return DEF_PRIORITY;
}

// Memory pressure simulation thread
static void *pressure_simulation(void *arg) {
    struct shrink_control sc = {
        .priority = DEF_PRIORITY,
        .total_scan = SHRINK_BATCH,
        .nr_scanned = 0,
        .gfp_mask = 0
    };

    while (pressure_sim.active) {
        sleep(1);  // Check every second
        
        int pressure_level = calculate_pressure_level();
        pthread_mutex_lock(&pressure_sim.lock);
        pressure_sim.level = pressure_level;
        pthread_mutex_unlock(&pressure_sim.lock);

        if (pressure_level > DEF_PRIORITY) {
            printf("Memory pressure detected (level %d)\n", pressure_level);
            sc.priority = pressure_level;
            
            // Try to free memory through all registered shrinkers
            pthread_mutex_lock(&shrinker_lock);
            for (int i = 0; i < nr_shrinkers; i++) {
                if (shrinkers[i] && (shrinkers[i]->flags & SHRINKER_ACTIVE)) {
                    unsigned long freed = do_shrink(shrinkers[i], &sc);
                    if (freed > 0) {
                        printf("Shrinker '%s' freed %lu objects\n",
                               shrinkers[i]->name, freed);
                    }
                }
            }
            pthread_mutex_unlock(&shrinker_lock);
        }
    }
    
    return NULL;
}

// Start memory pressure simulation
static int start_pressure_simulation(void) {
    pthread_mutex_lock(&pressure_sim.lock);
    
    if (pressure_sim.active) {
        pthread_mutex_unlock(&pressure_sim.lock);
        return SUCCESS;
    }

    pressure_sim.active = true;
    int ret = pthread_create(&pressure_sim.thread, NULL, pressure_simulation, NULL);
    
    pthread_mutex_unlock(&pressure_sim.lock);
    return ret == 0 ? SUCCESS : ERR_NO_MEMORY;
}

// Stop memory pressure simulation
static void stop_pressure_simulation(void) {
    pthread_mutex_lock(&pressure_sim.lock);
    
    if (pressure_sim.active) {
        pressure_sim.active = false;
        pthread_join(pressure_sim.thread, NULL);
    }
    
    pthread_mutex_unlock(&pressure_sim.lock);
}

/* Cleanup Functions */

// Free a cache type and all its objects
static void free_cache_type(struct cache_type *cache) {
    if (!cache)
        return;

    pthread_mutex_lock(&cache->lock);
    
    for (int i = 0; i < cache->total_objects; i++) {
        if (cache->objects[i]) {
            free(cache->objects[i]->data);
            free(cache->objects[i]);
        }
    }
    
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

// Free a shrinker
static void free_shrinker(struct shrinker *s) {
    if (!s)
        return;

    pthread_mutex_destroy(&s->lock);
    free(s);
}

// Cleanup all resources
static void cleanup_resources(void) {
    stop_pressure_simulation();

    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < nr_cache_types; i++) {
        if (cache_types[i]) {
            free_cache_type(cache_types[i]);
            cache_types[i] = NULL;
        }
    }
    pthread_mutex_unlock(&cache_lock);

    pthread_mutex_lock(&shrinker_lock);
    for (int i = 0; i < nr_shrinkers; i++) {
        if (shrinkers[i]) {
            free_shrinker(shrinkers[i]);
            shrinkers[i] = NULL;
        }
    }
    pthread_mutex_unlock(&shrinker_lock);
}

/* Main Function - Demonstration */

int main(void) {
    printf("Initializing Memory Shrinker System...\n");

    // Create some cache types with different sizes
    struct cache_type *caches[] = {
        create_cache_type("small_cache", 1024),      // 1KB objects
        create_cache_type("medium_cache", 4096),     // 4KB objects
        create_cache_type("large_cache", 16384),     // 16KB objects
        create_cache_type("huge_cache", 65536)       // 64KB objects
    };

    // Register shrinkers for each cache
    for (int i = 0; i < 4; i++) {
        if (caches[i]) {
            char shrinker_name[MAX_SHRINKER_NAME];
            snprintf(shrinker_name, MAX_SHRINKER_NAME, "%s_shrinker", caches[i]->name);
            register_cache_shrinker(caches[i], shrinker_name);
            printf("Registered shrinker for %s\n", caches[i]->name);
        }
    }

    // Start memory pressure simulation
    if (start_pressure_simulation() != SUCCESS) {
        fprintf(stderr, "Failed to start memory pressure simulation\n");
        cleanup_resources();
        return 1;
    }

    printf("\nSimulating memory allocation...\n");

    // Simulate memory allocation
    for (int round = 0; round < 5; round++) {
        printf("\nRound %d: Allocating objects...\n", round + 1);
        
        // Allocate objects in each cache
        for (int i = 0; i < 4; i++) {
            if (caches[i]) {
                int alloc_count = rand() % 100 + 50;  // Allocate 50-150 objects
                printf("Allocating %d objects in %s\n", alloc_count, caches[i]->name);
                
                for (int j = 0; j < alloc_count; j++) {
                    struct cache_object *obj = cache_alloc(caches[i]);
                    if (obj) {
                        // Simulate some work with the object
                        memset(obj->data, 0xff, obj->size);
                    }
                }
            }
        }

        // Print cache statistics
        printf("\nCache Statistics:\n");
        for (int i = 0; i < 4; i++) {
            if (caches[i]) {
                printf("%s: %d/%d objects used (%.1f%%)\n",
                       caches[i]->name,
                       caches[i]->used_objects,
                       caches[i]->total_objects,
                       (float)caches[i]->used_objects * 100 / caches[i]->total_objects);
            }
        }

        // Sleep to allow shrinker to work
        sleep(5);
    }

    printf("\nCleaning up...\n");
    cleanup_resources();

    return 0;
}
