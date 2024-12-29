/*
 * Page Writeback System Simulation
 * 
 * This program simulates the Linux kernel's page writeback mechanism, which is
 * responsible for writing dirty pages back to disk. It demonstrates the key
 * concepts of dirty page tracking, writeback throttling, and background writeback.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <sys/time.h>
#include <stdatomic.h>

/* Configuration Constants */
#define PAGE_SIZE           4096    // Size of each page in bytes
#define MAX_PAGES          1024    // Maximum number of physical pages
#define MAX_PROCESSES       64     // Maximum number of processes
#define MAX_WRITEBACK_THREADS 4   // Number of writeback threads
#define PAGE_SHIFT         12     // Page size shift (2^12 = 4096)
#define DIRTY_RATIO        20     // Default dirty ratio (percentage)
#define BG_DIRTY_RATIO     10     // Background writeback threshold ratio
#define WRITEBACK_INTERVAL 500    // Writeback interval in milliseconds
#define MAX_PAUSE          200    // Maximum pause time in milliseconds
#define BANDWIDTH_INTERVAL 200    // Bandwidth calculation interval in ms

/* Page Flags */
#define PAGE_FLAG_DIRTY     0x0001  // Page is dirty
#define PAGE_FLAG_WRITEBACK 0x0002  // Page is being written back
#define PAGE_FLAG_UPTODATE  0x0004  // Page content is valid
#define PAGE_FLAG_LOCKED    0x0008  // Page is locked
#define PAGE_FLAG_ACTIVE    0x0010  // Page is in active list
#define PAGE_FLAG_RECLAIM   0x0020  // Page can be reclaimed

/* Writeback Control Flags */
#define WB_SYNC_NONE       0       // Asynchronous writeback
#define WB_SYNC_ALL        1       // Synchronous writeback
#define WB_REASON_BACKGROUND 0     // Background writeback
#define WB_REASON_SYNC      1      // Synchronous writeback
#define WB_REASON_PERIODIC  2      // Periodic writeback

/* Forward Declarations */
struct page;
struct address_space;
struct writeback_control;
struct bdi_writeback;
struct backing_dev_info;

/* Type Definitions */

// Page structure representing a memory page
struct page {
    unsigned long flags;           // Page flags
    atomic_int _refcount;         // Reference count
    atomic_int _mapcount;         // Map count
    struct address_space *mapping; // Associated address space
    unsigned long index;          // Page index in address space
    void *virtual;                // Virtual address (simulated)
    pthread_mutex_t lock;         // Page lock
    struct list_head lru;         // LRU list entry
    unsigned long private;        // Private data
};

// Address space structure (represents a file's pages in memory)
struct address_space {
    struct backing_dev_info *backing_dev_info;  // Associated backing device
    unsigned long nrpages;                      // Number of pages
    unsigned long writeback_index;              // Writeback index
    pthread_rwlock_t i_mmap_rwlock;            // Protects the address space
    struct page **pages;                        // Page cache array
    unsigned long flags;                        // Address space flags
    void *private_data;                        // Private data
};

// Writeback control structure
struct writeback_control {
    long nr_to_write;            // Number of pages to write
    long pages_skipped;          // Pages skipped during writeback
    long nr_written;             // Number of pages written
    unsigned long start;         // Start time of writeback
    unsigned long bandwidth;     // Current bandwidth
    unsigned long older_than_this;  // Write pages older than this
    unsigned int for_background:1;  // Background writeback
    unsigned int for_sync:1;       // Synchronous writeback
    unsigned int range_cyclic:1;   // Range-cyclic writeback
    unsigned int no_cgroup_owner:1; // No cgroup ownership
};

// Backing device info structure
struct backing_dev_info {
    unsigned long ra_pages;      // Readahead window size
    unsigned long io_pages;      // Number of IO pages
    unsigned long dirty_limit;   // Dirty page limit
    atomic_long_t dirty_pages;   // Current dirty pages
    atomic_long_t written_pages; // Total written pages
    struct bdi_writeback wb;     // Associated writeback control
    pthread_mutex_t wb_lock;     // Protects writeback lists
};

// Writeback control structure
struct bdi_writeback {
    struct backing_dev_info *bdi;    // Associated backing device
    unsigned long state;             // Writeback state
    atomic_long_t writeback_pages;   // Pages under writeback
    atomic_long_t dirty_pages;       // Dirty pages count
    unsigned long avg_write_bandwidth;// Average write bandwidth
    struct list_head b_dirty;        // Dirty pages list
    struct list_head b_io;           // IO pages list
    struct list_head b_more_io;      // More IO pages list
    pthread_mutex_t list_lock;       // Protects lists
};

// List management structure
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* Global Variables */
static struct page *pages[MAX_PAGES];
static struct backing_dev_info *global_bdi;
static pthread_t writeback_threads[MAX_WRITEBACK_THREADS];
static bool system_running = true;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

/* List Management Functions */

static inline void list_init(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new,
                            struct list_head *prev,
                            struct list_head *next) {
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head) {
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head) {
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

/* Page Management Functions */

// Initialize a page structure
static void page_init(struct page *page) {
    memset(page, 0, sizeof(*page));
    atomic_init(&page->_refcount, 1);
    atomic_init(&page->_mapcount, 0);
    pthread_mutex_init(&page->lock, NULL);
    list_init(&page->lru);
}

// Allocate a new page
static struct page *alloc_page(void) {
    struct page *page = malloc(sizeof(struct page));
    if (!page)
        return NULL;
    page_init(page);
    return page;
}

// Free a page
static void free_page(struct page *page) {
    if (!page)
        return;
    pthread_mutex_destroy(&page->lock);
    free(page);
}

// Lock a page
static void lock_page(struct page *page) {
    pthread_mutex_lock(&page->lock);
    page->flags |= PAGE_FLAG_LOCKED;
}

// Unlock a page
static void unlock_page(struct page *page) {
    page->flags &= ~PAGE_FLAG_LOCKED;
    pthread_mutex_unlock(&page->lock);
}

// Mark a page as dirty
static void set_page_dirty(struct page *page) {
    lock_page(page);
    if (!(page->flags & PAGE_FLAG_DIRTY)) {
        page->flags |= PAGE_FLAG_DIRTY;
        if (page->mapping) {
            atomic_fetch_add(&page->mapping->backing_dev_info->dirty_pages, 1);
        }
    }
    unlock_page(page);
}

// Clear a page's dirty status
static void clear_page_dirty(struct page *page) {
    lock_page(page);
    if (page->flags & PAGE_FLAG_DIRTY) {
        page->flags &= ~PAGE_FLAG_DIRTY;
        if (page->mapping) {
            atomic_fetch_sub(&page->mapping->backing_dev_info->dirty_pages, 1);
        }
    }
    unlock_page(page);
}

/* Writeback Control Functions */

// Initialize writeback control structure
static void wbc_init(struct writeback_control *wbc, int reason) {
    memset(wbc, 0, sizeof(*wbc));
    wbc->nr_to_write = LONG_MAX;
    wbc->start = time(NULL);
    
    switch (reason) {
        case WB_REASON_BACKGROUND:
            wbc->for_background = 1;
            break;
        case WB_REASON_SYNC:
            wbc->for_sync = 1;
            break;
        default:
            break;
    }
}

// Update writeback statistics
static void wb_update_stats(struct bdi_writeback *wb,
                          unsigned long pages_written,
                          unsigned long elapsed) {
    if (elapsed) {
        unsigned long bandwidth = (pages_written * 1000) / elapsed;
        wb->avg_write_bandwidth = (wb->avg_write_bandwidth + bandwidth) / 2;
    }
    
    atomic_fetch_add(&wb->bdi->written_pages, pages_written);
}

/* Writeback Thread Functions */

// Background writeback thread
static void *background_writeout(void *data) {
    struct bdi_writeback *wb = data;
    struct writeback_control wbc;
    
    while (system_running) {
        // Sleep for the writeback interval
        usleep(WRITEBACK_INTERVAL * 1000);
        
        // Check if writeback is needed
        if (atomic_load(&wb->bdi->dirty_pages) > 
            (wb->bdi->dirty_limit * BG_DIRTY_RATIO / 100)) {
            
            // Initialize writeback control
            wbc_init(&wbc, WB_REASON_BACKGROUND);
            
            // Perform writeback
            pthread_mutex_lock(&wb->list_lock);
            struct list_head *pos, *next;
            list_for_each_safe(pos, next, &wb->b_dirty) {
                struct page *page = list_entry(pos, struct page, lru);
                
                if (!(page->flags & PAGE_FLAG_DIRTY))
                    continue;
                
                // Simulate page writeback
                page->flags |= PAGE_FLAG_WRITEBACK;
                list_move(&page->lru, &wb->b_io);
                
                // Simulate I/O completion
                usleep(1000); // Simulate 1ms I/O time
                clear_page_dirty(page);
                page->flags &= ~PAGE_FLAG_WRITEBACK;
                list_move(&page->lru, &wb->b_more_io);
                
                wbc.nr_written++;
                if (--wbc.nr_to_write <= 0)
                    break;
            }
            pthread_mutex_unlock(&wb->list_lock);
            
            // Update statistics
            if (wbc.nr_written) {
                unsigned long elapsed = time(NULL) - wbc.start;
                wb_update_stats(wb, wbc.nr_written, elapsed);
            }
        }
    }
    
    return NULL;
}

/* Initialization Functions */

// Initialize the backing device info
static struct backing_dev_info *bdi_init(void) {
    struct backing_dev_info *bdi = malloc(sizeof(*bdi));
    if (!bdi)
        return NULL;
        
    memset(bdi, 0, sizeof(*bdi));
    atomic_init(&bdi->dirty_pages, 0);
    atomic_init(&bdi->written_pages, 0);
    bdi->dirty_limit = MAX_PAGES;
    pthread_mutex_init(&bdi->wb_lock, NULL);
    
    // Initialize writeback control
    struct bdi_writeback *wb = &bdi->wb;
    wb->bdi = bdi;
    atomic_init(&wb->writeback_pages, 0);
    atomic_init(&wb->dirty_pages, 0);
    list_init(&wb->b_dirty);
    list_init(&wb->b_io);
    list_init(&wb->b_more_io);
    pthread_mutex_init(&wb->list_lock, NULL);
    
    return bdi;
}

// Initialize the page writeback system
static int writeback_init(void) {
    int i;
    
    // Initialize global backing device
    global_bdi = bdi_init();
    if (!global_bdi)
        return -ENOMEM;
    
    // Allocate pages
    for (i = 0; i < MAX_PAGES; i++) {
        pages[i] = alloc_page();
        if (!pages[i])
            goto cleanup;
    }
    
    // Start writeback threads
    for (i = 0; i < MAX_WRITEBACK_THREADS; i++) {
        if (pthread_create(&writeback_threads[i], NULL,
                         background_writeout, &global_bdi->wb) != 0)
            goto cleanup;
    }
    
    return 0;
    
cleanup:
    while (--i >= 0)
        free_page(pages[i]);
    free(global_bdi);
    return -ENOMEM;
}

// Cleanup the page writeback system
static void writeback_cleanup(void) {
    int i;
    
    // Stop the system
    system_running = false;
    
    // Wait for writeback threads to finish
    for (i = 0; i < MAX_WRITEBACK_THREADS; i++)
        pthread_join(writeback_threads[i], NULL);
    
    // Free pages
    for (i = 0; i < MAX_PAGES; i++)
        free_page(pages[i]);
    
    // Cleanup backing device
    free(global_bdi);
}

/* Simulation Functions */

// Simulate page dirtying
static void simulate_page_dirtying(void) {
    int i;
    struct bdi_writeback *wb = &global_bdi->wb;
    
    printf("Starting page dirtying simulation...\n");
    
    for (i = 0; i < MAX_PAGES / 2; i++) {
        struct page *page = pages[i];
        
        // Dirty the page
        set_page_dirty(page);
        
        // Add to dirty list
        pthread_mutex_lock(&wb->list_lock);
        list_add_tail(&page->lru, &wb->b_dirty);
        pthread_mutex_unlock(&wb->list_lock);
        
        // Simulate some work
        usleep(10000); // 10ms
        
        // Print progress every 100 pages
        if ((i + 1) % 100 == 0) {
            printf("Dirtied %d pages, current dirty pages: %ld\n",
                   i + 1, atomic_load(&global_bdi->dirty_pages));
        }
    }
}

// Monitor writeback progress
static void monitor_writeback(void) {
    time_t start = time(NULL);
    unsigned long last_written = 0;
    
    printf("\nMonitoring writeback progress...\n");
    
    while (atomic_load(&global_bdi->dirty_pages) > 0) {
        unsigned long current_written = atomic_load(&global_bdi->written_pages);
        unsigned long written_delta = current_written - last_written;
        time_t elapsed = time(NULL) - start;
        
        printf("Time: %lds, Dirty pages: %ld, Written: %ld (%ld/s), "
               "Bandwidth: %ld KB/s\n",
               elapsed,
               atomic_load(&global_bdi->dirty_pages),
               current_written,
               written_delta,
               (written_delta * PAGE_SIZE) / 1024);
        
        last_written = current_written;
        sleep(1);
    }
}

/* Main Function */

int main(void) {
    printf("Starting Page Writeback Simulation\n");
    printf("Configuration:\n");
    printf("- Page Size: %d bytes\n", PAGE_SIZE);
    printf("- Max Pages: %d\n", MAX_PAGES);
    printf("- Writeback Threads: %d\n", MAX_WRITEBACK_THREADS);
    printf("- Dirty Ratio: %d%%\n", DIRTY_RATIO);
    printf("- Background Dirty Ratio: %d%%\n", BG_DIRTY_RATIO);
    printf("\n");
    
    if (writeback_init() != 0) {
        fprintf(stderr, "Failed to initialize writeback system\n");
        return 1;
    }
    
    // Run simulation
    simulate_page_dirtying();
    monitor_writeback();
    
    // Cleanup
    writeback_cleanup();
    
    printf("\nSimulation completed.\n");
    return 0;
}
