/*
 * Memory Swap System Simulation
 * 
 * This program simulates a memory management system with swap functionality,
 * similar to the Linux kernel's swap system but simplified for educational
 * purposes. It demonstrates how pages are moved between memory and swap space.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

/* Configuration Constants */
#define PAGE_SIZE           4096    // Size of each page in bytes
#define MAX_PAGES          1024     // Maximum number of pages in memory
#define SWAP_FILE_PAGES    4096     // Number of pages in swap file
#define MAX_PROCESSES       100     // Maximum number of simulated processes
#define PAGE_CLUSTER         3      // Number of pages to swap at once (2^3 = 8)
#define SWAP_FILENAME     "swap_simulation.swp"

/* Page States */
#define PAGE_STATE_FREE     0       // Page is free
#define PAGE_STATE_USED     1       // Page is in use
#define PAGE_STATE_DIRTY    2       // Page needs to be written to swap
#define PAGE_STATE_CLEAN    3       // Page is clean (matches swap)
#define PAGE_STATE_SWAPPED  4       // Page is in swap file

/* LRU List Types */
#define LRU_INACTIVE_ANON   0
#define LRU_ACTIVE_ANON     1
#define LRU_INACTIVE_FILE   2
#define LRU_ACTIVE_FILE     3
#define LRU_UNEVICTABLE     4
#define NR_LRU_LISTS        5

/* Error Codes */
#define SUCCESS             0
#define ERR_NO_MEMORY     -1
#define ERR_NO_SWAP       -2
#define ERR_INVALID_PAGE  -3
#define ERR_IO_ERROR      -4

/* Structures */

// Represents a memory page
struct page {
    unsigned long flags;            // Page flags
    unsigned int state;            // Page state
    unsigned int count;           // Reference count
    unsigned long accessed;       // Last access time
    unsigned long age;           // Page age for swap selection
    int swap_index;             // Location in swap file (-1 if not swapped)
    void *data;                // Actual page data
    struct list_head lru;      // LRU list linkage
    int owner_pid;            // Process ID that owns this page
};

// LRU list structure
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

// Process structure
struct process {
    int pid;                    // Process ID
    char name[64];             // Process name
    struct page *pages[MAX_PAGES]; // Pages owned by this process
    int page_count;            // Number of pages owned
    bool active;               // Whether process is active
};

// Memory zone structure
struct zone {
    struct page *pages[MAX_PAGES];
    struct list_head lru[NR_LRU_LISTS];
    unsigned long nr_pages;
    unsigned long nr_free_pages;
    pthread_mutex_t lock;
};

// Swap area structure
struct swap_area {
    int fd;                     // Swap file descriptor
    unsigned long *bitmap;      // Bitmap of used swap slots
    unsigned long nr_slots;     // Number of slots in swap
    pthread_mutex_t lock;
};

/* Global Variables */
static struct zone memory_zone;
static struct swap_area swap_area;
static struct process processes[MAX_PROCESSES];
static int next_pid = 1;
static pthread_mutex_t process_lock = PTHREAD_MUTEX_INITIALIZER;

/* List manipulation functions */
static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new,
                            struct list_head *prev,
                            struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

/* Page Management Functions */

// Initialize a new page
static void page_init(struct page *page)
{
    memset(page, 0, sizeof(struct page));
    page->state = PAGE_STATE_FREE;
    page->count = 0;
    page->swap_index = -1;
    page->data = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    INIT_LIST_HEAD(&page->lru);
    page->owner_pid = -1;
}

// Allocate a new page
static struct page *alloc_page(void)
{
    struct page *page = NULL;
    int i;

    pthread_mutex_lock(&memory_zone.lock);
    
    // First try to find a free page
    for (i = 0; i < MAX_PAGES; i++) {
        if (memory_zone.pages[i]->state == PAGE_STATE_FREE) {
            page = memory_zone.pages[i];
            page->state = PAGE_STATE_USED;
            page->count = 1;
            page->accessed = time(NULL);
            memory_zone.nr_free_pages--;
            break;
        }
    }

    pthread_mutex_unlock(&memory_zone.lock);
    return page;
}

// Free a page
static void free_page(struct page *page)
{
    if (!page)
        return;

    pthread_mutex_lock(&memory_zone.lock);
    
    if (page->state != PAGE_STATE_FREE) {
        // Remove from LRU if necessary
        if (!list_empty(&page->lru))
            list_del(&page->lru);

        // Clear page data
        memset(page->data, 0, PAGE_SIZE);
        page->state = PAGE_STATE_FREE;
        page->count = 0;
        page->swap_index = -1;
        page->owner_pid = -1;
        memory_zone.nr_free_pages++;
    }

    pthread_mutex_unlock(&memory_zone.lock);
}

/* Swap Management Functions */

// Initialize swap area
static int swap_init(void)
{
    swap_area.fd = open(SWAP_FILENAME, O_RDWR | O_CREAT, 0600);
    if (swap_area.fd < 0)
        return ERR_IO_ERROR;

    // Allocate bitmap for swap slots
    swap_area.nr_slots = SWAP_FILE_PAGES;
    size_t bitmap_size = (SWAP_FILE_PAGES + 7) / 8;
    swap_area.bitmap = calloc(1, bitmap_size);
    if (!swap_area.bitmap) {
        close(swap_area.fd);
        return ERR_NO_MEMORY;
    }

    // Initialize swap file
    if (ftruncate(swap_area.fd, SWAP_FILE_PAGES * PAGE_SIZE) < 0) {
        free(swap_area.bitmap);
        close(swap_area.fd);
        return ERR_IO_ERROR;
    }

    pthread_mutex_init(&swap_area.lock, NULL);
    return SUCCESS;
}

// Find a free swap slot
static int get_swap_slot(void)
{
    int slot = -1;
    unsigned long *bitmap = swap_area.bitmap;
    int nr_slots = swap_area.nr_slots;

    pthread_mutex_lock(&swap_area.lock);

    // Find first free bit in bitmap
    for (int i = 0; i < nr_slots; i++) {
        if (!(bitmap[i / 64] & (1UL << (i % 64)))) {
            bitmap[i / 64] |= (1UL << (i % 64));
            slot = i;
            break;
        }
    }

    pthread_mutex_unlock(&swap_area.lock);
    return slot;
}

// Release a swap slot
static void put_swap_slot(int slot)
{
    if (slot < 0 || slot >= swap_area.nr_slots)
        return;

    pthread_mutex_lock(&swap_area.lock);
    swap_area.bitmap[slot / 64] &= ~(1UL << (slot % 64));
    pthread_mutex_unlock(&swap_area.lock);
}

// Write page to swap
static int write_to_swap(struct page *page)
{
    int slot = get_swap_slot();
    if (slot < 0)
        return ERR_NO_SWAP;

    off_t offset = (off_t)slot * PAGE_SIZE;
    ssize_t written = pwrite(swap_area.fd, page->data, PAGE_SIZE, offset);
    
    if (written != PAGE_SIZE) {
        put_swap_slot(slot);
        return ERR_IO_ERROR;
    }

    page->swap_index = slot;
    page->state = PAGE_STATE_CLEAN;
    return SUCCESS;
}

// Read page from swap
static int read_from_swap(struct page *page)
{
    if (page->swap_index < 0)
        return ERR_INVALID_PAGE;

    off_t offset = (off_t)page->swap_index * PAGE_SIZE;
    ssize_t bytes_read = pread(swap_area.fd, page->data, PAGE_SIZE, offset);
    
    if (bytes_read != PAGE_SIZE)
        return ERR_IO_ERROR;

    put_swap_slot(page->swap_index);
    page->swap_index = -1;
    page->state = PAGE_STATE_CLEAN;
    return SUCCESS;
}

/* LRU Management Functions */

// Add page to LRU list
static void add_page_to_lru(struct page *page, int lru_type)
{
    pthread_mutex_lock(&memory_zone.lock);
    list_add_tail(&page->lru, &memory_zone.lru[lru_type]);
    pthread_mutex_unlock(&memory_zone.lock);
}

// Remove page from LRU list
static void del_page_from_lru(struct page *page)
{
    pthread_mutex_lock(&memory_zone.lock);
    if (!list_empty(&page->lru))
        list_del(&page->lru);
    pthread_mutex_unlock(&memory_zone.lock);
}

// Move page to active list
static void activate_page(struct page *page)
{
    del_page_from_lru(page);
    add_page_to_lru(page, LRU_ACTIVE_ANON);
    page->accessed = time(NULL);
}

// Move page to inactive list
static void deactivate_page(struct page *page)
{
    del_page_from_lru(page);
    add_page_to_lru(page, LRU_INACTIVE_ANON);
}

/* Process Management Functions */

// Create a new process
static struct process *create_process(const char *name)
{
    pthread_mutex_lock(&process_lock);
    
    int pid = next_pid++;
    struct process *proc = &processes[pid % MAX_PROCESSES];
    
    proc->pid = pid;
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    proc->page_count = 0;
    proc->active = true;
    
    pthread_mutex_unlock(&process_lock);
    return proc;
}

// Allocate pages for process
static int allocate_process_pages(struct process *proc, int num_pages)
{
    if (proc->page_count + num_pages > MAX_PAGES)
        return ERR_NO_MEMORY;

    for (int i = 0; i < num_pages; i++) {
        struct page *page = alloc_page();
        if (!page)
            return ERR_NO_MEMORY;

        page->owner_pid = proc->pid;
        proc->pages[proc->page_count++] = page;
        add_page_to_lru(page, LRU_ACTIVE_ANON);
    }

    return SUCCESS;
}

/* Page Reclaim Functions */

// Check if a page can be evicted
static bool can_evict_page(struct page *page)
{
    return page->state == PAGE_STATE_CLEAN ||
           page->state == PAGE_STATE_DIRTY;
}

// Find pages to evict
static int find_pages_to_evict(struct page **pages, int nr_to_find)
{
    int nr_found = 0;
    struct list_head *lru = &memory_zone.lru[LRU_INACTIVE_ANON];
    struct list_head *entry;
    
    pthread_mutex_lock(&memory_zone.lock);

    list_for_each(entry, lru) {
        if (nr_found >= nr_to_find)
            break;

        struct page *page = container_of(entry, struct page, lru);
        if (can_evict_page(page))
            pages[nr_found++] = page;
    }

    pthread_mutex_unlock(&memory_zone.lock);
    return nr_found;
}

// Swap out pages
static int swap_out_pages(int nr_pages)
{
    struct page *pages[32];  // Max pages to swap at once
    int nr_to_swap = min(nr_pages, 32);
    int nr_found = find_pages_to_evict(pages, nr_to_swap);
    int nr_swapped = 0;

    for (int i = 0; i < nr_found; i++) {
        struct page *page = pages[i];
        
        if (page->state == PAGE_STATE_DIRTY) {
            if (write_to_swap(page) == SUCCESS) {
                del_page_from_lru(page);
                page->state = PAGE_STATE_SWAPPED;
                nr_swapped++;
            }
        }
    }

    return nr_swapped;
}

/* Memory Pressure Simulation */

// Simulate memory pressure
static void *memory_pressure_thread(void *arg)
{
    while (1) {
        sleep(1);  // Check every second

        pthread_mutex_lock(&memory_zone.lock);
        int free_pages = memory_zone.nr_free_pages;
        pthread_mutex_unlock(&memory_zone.lock);

        // If free pages are less than 10% of total, trigger page reclaim
        if (free_pages < MAX_PAGES / 10) {
            printf("Memory pressure detected. Free pages: %d\n", free_pages);
            int nr_to_swap = (MAX_PAGES / 10) - free_pages;
            int nr_swapped = swap_out_pages(nr_to_swap);
            printf("Swapped out %d pages\n", nr_swapped);
        }
    }
    return NULL;
}

/* Initialization and Cleanup */

// Initialize the memory management system
static int mm_init(void)
{
    // Initialize memory zone
    pthread_mutex_init(&memory_zone.lock, NULL);
    memory_zone.nr_pages = MAX_PAGES;
    memory_zone.nr_free_pages = MAX_PAGES;

    for (int i = 0; i < NR_LRU_LISTS; i++)
        INIT_LIST_HEAD(&memory_zone.lru[i]);

    // Allocate and initialize pages
    for (int i = 0; i < MAX_PAGES; i++) {
        memory_zone.pages[i] = malloc(sizeof(struct page));
        if (!memory_zone.pages[i])
            return ERR_NO_MEMORY;
        page_init(memory_zone.pages[i]);
    }

    // Initialize swap area
    if (swap_init() != SUCCESS)
        return ERR_NO_SWAP;

    // Start memory pressure thread
    pthread_t pressure_thread;
    if (pthread_create(&pressure_thread, NULL, memory_pressure_thread, NULL) != 0)
        return ERR_NO_MEMORY;

    return SUCCESS;
}

// Cleanup memory management system
static void mm_cleanup(void)
{
    // Clean up swap area
    if (swap_area.fd >= 0) {
        close(swap_area.fd);
        unlink(SWAP_FILENAME);
    }
    free(swap_area.bitmap);

    // Free all pages
    for (int i = 0; i < MAX_PAGES; i++) {
        if (memory_zone.pages[i]) {
            free(memory_zone.pages[i]->data);
            free(memory_zone.pages[i]);
        }
    }

    pthread_mutex_destroy(&memory_zone.lock);
    pthread_mutex_destroy(&swap_area.lock);
}

/* Main Function - Demonstration */

int main(void)
{
    printf("Initializing Memory Management System...\n");
    
    if (mm_init() != SUCCESS) {
        fprintf(stderr, "Failed to initialize memory management system\n");
        return 1;
    }

    printf("Creating test processes...\n");

    // Create some test processes
    struct process *procs[5];
    const char *proc_names[] = {
        "System",
        "Web Browser",
        "Text Editor",
        "Media Player",
        "Background Service"
    };

    for (int i = 0; i < 5; i++) {
        procs[i] = create_process(proc_names[i]);
        printf("Created process: %s (PID: %d)\n", procs[i]->name, procs[i]->pid);

        // Allocate random number of pages to each process
        int num_pages = rand() % 100 + 50;  // 50-150 pages
        if (allocate_process_pages(procs[i], num_pages) == SUCCESS) {
            printf("Allocated %d pages to %s\n", num_pages, procs[i]->name);
        } else {
            printf("Failed to allocate pages to %s\n", procs[i]->name);
        }
    }

    printf("\nMemory Management System Statistics:\n");
    printf("Total Pages: %lu\n", memory_zone.nr_pages);
    printf("Free Pages: %lu\n", memory_zone.nr_free_pages);
    printf("Swap Slots: %lu\n", swap_area.nr_slots);

    // Let the system run for a while to demonstrate memory pressure
    printf("\nRunning simulation for 30 seconds...\n");
    sleep(30);

    printf("\nCleaning up...\n");
    mm_cleanup();

    return 0;
}
