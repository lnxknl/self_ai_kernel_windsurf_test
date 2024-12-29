/*
 * File Readahead System Simulation
 * 
 * This program simulates a file readahead system similar to the Linux kernel's
 * readahead mechanism. It demonstrates how readahead can improve file access
 * performance by prefetching data before it is explicitly requested.
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
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>

/* Configuration Constants */
#define PAGE_SIZE           4096    // Size of each page in bytes
#define MAX_PAGES          1024     // Maximum number of pages in cache
#define MAX_FILES           100     // Maximum number of open files
#define DEFAULT_RA_PAGES    32      // Default readahead window size
#define MAX_RA_PAGES       128      // Maximum readahead window size
#define MIN_RA_PAGES         8      // Minimum readahead window size
#define CACHE_LINE_SIZE     64      // Cache line size for alignment
#define MAX_FILENAME       256      // Maximum filename length
#define READAHEAD_BATCH     16      // Number of pages to read at once

/* Page States */
#define PAGE_STATE_EMPTY    0       // Page is empty
#define PAGE_STATE_READING  1       // Page is being read
#define PAGE_STATE_VALID    2       // Page contains valid data
#define PAGE_STATE_DIRTY    3       // Page needs to be written back

/* Error Codes */
#define SUCCESS             0
#define ERR_NO_MEMORY     -1
#define ERR_INVALID_PARAM -2
#define ERR_IO_ERROR      -3
#define ERR_NOT_FOUND     -4
#define ERR_BUSY          -5

/* Readahead Flags */
#define RA_FLAG_SYNC      0x1      // Synchronous readahead
#define RA_FLAG_ASYNC     0x2      // Asynchronous readahead
#define RA_FLAG_RANDOM    0x4      // Random access pattern
#define RA_FLAG_SEQUENTIAL 0x8      // Sequential access pattern

/* Forward Declarations */
struct page;
struct page_cache;
struct file_ra_state;
struct readahead_control;
struct file_handle;

/* Type Definitions */

// Page structure representing a cached page
struct page {
    unsigned long index;           // Page index in file
    unsigned int state;           // Page state
    unsigned long flags;         // Page flags
    void *data;                // Page data
    size_t size;              // Actual data size
    struct page *next;       // Next page in LRU list
    pthread_mutex_t lock;   // Page lock
};

// Page cache structure
struct page_cache {
    struct page *pages[MAX_PAGES];
    struct page *lru_head;         // Head of LRU list
    struct page *lru_tail;         // Tail of LRU list
    unsigned long nr_pages;       // Number of pages in cache
    pthread_mutex_t lock;        // Cache lock
    pthread_cond_t cond;        // Condition for async operations
};

// File readahead state
struct file_ra_state {
    unsigned long start;          // Start of current window
    unsigned long size;          // Size of current window
    unsigned long async_size;    // Size of async portion
    unsigned long ra_pages;     // Number of pages to read ahead
    unsigned long mmap_miss;    // Cache misses
    unsigned long prev_pos;     // Previous read position
    unsigned long flags;       // Readahead flags
    unsigned int height;      // Height in page tree
    unsigned int density;    // Density of pages in window
};

// Readahead control structure
struct readahead_control {
    struct file_handle *file;    // Associated file
    struct page_cache *cache;   // Page cache
    struct file_ra_state *ra;  // Readahead state
    unsigned long index;      // Current page index
    unsigned long nr_pages;  // Number of pages to read
    bool async;             // Asynchronous operation
};

// File handle structure
struct file_handle {
    int fd;                     // File descriptor
    char name[MAX_FILENAME];   // File name
    off_t size;               // File size
    struct file_ra_state ra; // Readahead state
    struct page_cache *cache;// Page cache
    pthread_mutex_t lock;   // File lock
};

/* Global Variables */
static struct page_cache global_cache;
static struct file_handle *open_files[MAX_FILES];
static int nr_open_files = 0;
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* Page Management Functions */

// Initialize a page
static void page_init(struct page *page) {
    page->index = 0;
    page->state = PAGE_STATE_EMPTY;
    page->flags = 0;
    page->size = 0;
    page->next = NULL;
    page->data = aligned_alloc(CACHE_LINE_SIZE, PAGE_SIZE);
    pthread_mutex_init(&page->lock, NULL);
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
    free(page->data);
    free(page);
}

/* Page Cache Management */

// Initialize page cache
static void cache_init(struct page_cache *cache) {
    pthread_mutex_init(&cache->lock, NULL);
    pthread_cond_init(&cache->cond, NULL);
    cache->nr_pages = 0;
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    
    // Allocate initial pages
    for (int i = 0; i < MAX_PAGES; i++) {
        cache->pages[i] = alloc_page();
        if (!cache->pages[i])
            break;
        cache->nr_pages++;
    }
}

// Add page to LRU list
static void add_to_lru(struct page_cache *cache, struct page *page) {
    pthread_mutex_lock(&cache->lock);
    
    if (!cache->lru_head) {
        cache->lru_head = cache->lru_tail = page;
    } else {
        cache->lru_tail->next = page;
        cache->lru_tail = page;
    }
    page->next = NULL;
    
    pthread_mutex_unlock(&cache->lock);
}

// Remove page from LRU list
static void remove_from_lru(struct page_cache *cache, struct page *page) {
    pthread_mutex_lock(&cache->lock);
    
    struct page *curr = cache->lru_head;
    struct page *prev = NULL;
    
    while (curr) {
        if (curr == page) {
            if (prev)
                prev->next = curr->next;
            else
                cache->lru_head = curr->next;
                
            if (cache->lru_tail == curr)
                cache->lru_tail = prev;
                
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&cache->lock);
}

// Find page in cache
static struct page *find_page(struct page_cache *cache, unsigned long index) {
    pthread_mutex_lock(&cache->lock);
    
    for (unsigned long i = 0; i < cache->nr_pages; i++) {
        struct page *page = cache->pages[i];
        if (page->index == index && page->state != PAGE_STATE_EMPTY) {
            pthread_mutex_unlock(&cache->lock);
            return page;
        }
    }
    
    pthread_mutex_unlock(&cache->lock);
    return NULL;
}

// Get a free page from cache
static struct page *get_free_page(struct page_cache *cache) {
    pthread_mutex_lock(&cache->lock);
    
    // First try to find an empty page
    for (unsigned long i = 0; i < cache->nr_pages; i++) {
        struct page *page = cache->pages[i];
        if (page->state == PAGE_STATE_EMPTY) {
            pthread_mutex_unlock(&cache->lock);
            return page;
        }
    }
    
    // If no empty pages, evict from LRU
    if (cache->lru_head) {
        struct page *victim = cache->lru_head;
        remove_from_lru(cache, victim);
        victim->state = PAGE_STATE_EMPTY;
        pthread_mutex_unlock(&cache->lock);
        return victim;
    }
    
    pthread_mutex_unlock(&cache->lock);
    return NULL;
}

/* Readahead State Management */

// Initialize readahead state
static void ra_state_init(struct file_ra_state *ra) {
    ra->start = 0;
    ra->size = 0;
    ra->async_size = 0;
    ra->ra_pages = DEFAULT_RA_PAGES;
    ra->mmap_miss = 0;
    ra->prev_pos = -1;
    ra->flags = RA_FLAG_SEQUENTIAL;
    ra->height = 0;
    ra->density = 0;
}

// Calculate next readahead size
static unsigned long get_next_ra_size(struct file_ra_state *ra) {
    unsigned long size = ra->size;
    
    if (size >= MAX_RA_PAGES)
        return MAX_RA_PAGES;
    
    if (size <= 16)
        return size * 4;
    if (size <= 32)
        return size * 2;
    return size + size/2;
}

// Submit readahead request
static int submit_readahead(struct readahead_control *rac) {
    struct file_handle *file = rac->file;
    unsigned long index = rac->index;
    unsigned long nr_pages = rac->nr_pages;
    bool async = rac->async;
    
    printf("Submitting readahead request: index=%lu, pages=%lu, async=%d\n",
           index, nr_pages, async);
           
    // Read pages in batches
    for (unsigned long i = 0; i < nr_pages; i += READAHEAD_BATCH) {
        unsigned long batch_size = min(READAHEAD_BATCH, nr_pages - i);
        
        for (unsigned long j = 0; j < batch_size; j++) {
            struct page *page = get_free_page(rac->cache);
            if (!page)
                return ERR_NO_MEMORY;
                
            page->index = index + i + j;
            page->state = PAGE_STATE_READING;
            
            // Simulate reading from file
            off_t offset = page->index * PAGE_SIZE;
            if (pread(file->fd, page->data, PAGE_SIZE, offset) != PAGE_SIZE) {
                page->state = PAGE_STATE_EMPTY;
                return ERR_IO_ERROR;
            }
            
            page->state = PAGE_STATE_VALID;
            page->size = PAGE_SIZE;
            add_to_lru(rac->cache, page);
        }
        
        // If synchronous, wait a bit to simulate I/O time
        if (!async)
            usleep(1000);  // 1ms delay
    }
    
    return SUCCESS;
}

/* File Operations */

// Open a file with readahead
static struct file_handle *ra_open(const char *filename) {
    pthread_mutex_lock(&files_lock);
    
    if (nr_open_files >= MAX_FILES) {
        pthread_mutex_unlock(&files_lock);
        return NULL;
    }
    
    struct file_handle *file = malloc(sizeof(struct file_handle));
    if (!file) {
        pthread_mutex_unlock(&files_lock);
        return NULL;
    }
    
    file->fd = open(filename, O_RDONLY);
    if (file->fd < 0) {
        free(file);
        pthread_mutex_unlock(&files_lock);
        return NULL;
    }
    
    strncpy(file->name, filename, MAX_FILENAME - 1);
    struct stat st;
    fstat(file->fd, &st);
    file->size = st.st_size;
    
    ra_state_init(&file->ra);
    file->cache = &global_cache;
    pthread_mutex_init(&file->lock, NULL);
    
    open_files[nr_open_files++] = file;
    pthread_mutex_unlock(&files_lock);
    return file;
}

// Close a file
static void ra_close(struct file_handle *file) {
    if (!file)
        return;
        
    pthread_mutex_lock(&files_lock);
    
    // Remove from open files array
    for (int i = 0; i < nr_open_files; i++) {
        if (open_files[i] == file) {
            memmove(&open_files[i], &open_files[i + 1],
                   (nr_open_files - i - 1) * sizeof(struct file_handle *));
            nr_open_files--;
            break;
        }
    }
    
    close(file->fd);
    pthread_mutex_destroy(&file->lock);
    free(file);
    
    pthread_mutex_unlock(&files_lock);
}

// Read from file with readahead
static ssize_t ra_read(struct file_handle *file, void *buf,
                      size_t count, off_t offset) {
    if (!file || !buf || offset < 0)
        return ERR_INVALID_PARAM;
        
    pthread_mutex_lock(&file->lock);
    
    // Calculate page indices
    unsigned long start_index = offset / PAGE_SIZE;
    unsigned long end_index = (offset + count - 1) / PAGE_SIZE;
    unsigned long nr_pages = end_index - start_index + 1;
    
    // Check if we need to trigger readahead
    struct readahead_control rac = {
        .file = file,
        .cache = file->cache,
        .ra = &file->ra,
        .index = start_index,
        .nr_pages = nr_pages,
        .async = false
    };
    
    // If this is a sequential read, trigger async readahead
    if (file->ra.prev_pos >= 0 &&
        offset == file->ra.prev_pos + PAGE_SIZE) {
        rac.async = true;
        rac.nr_pages = get_next_ra_size(&file->ra);
        submit_readahead(&rac);
    }
    
    // Read requested pages
    size_t bytes_read = 0;
    unsigned long curr_index = start_index;
    
    while (bytes_read < count) {
        struct page *page = find_page(file->cache, curr_index);
        if (!page) {
            // Page not in cache, trigger synchronous readahead
            rac.async = false;
            rac.index = curr_index;
            rac.nr_pages = 1;
            if (submit_readahead(&rac) != SUCCESS) {
                pthread_mutex_unlock(&file->lock);
                return ERR_IO_ERROR;
            }
            page = find_page(file->cache, curr_index);
        }
        
        // Copy data from page to user buffer
        size_t page_offset = offset % PAGE_SIZE;
        size_t bytes_to_copy = min(PAGE_SIZE - page_offset,
                                 count - bytes_read);
        memcpy(buf + bytes_read,
               page->data + page_offset,
               bytes_to_copy);
               
        bytes_read += bytes_to_copy;
        offset += bytes_to_copy;
        curr_index = offset / PAGE_SIZE;
    }
    
    // Update readahead state
    file->ra.prev_pos = offset;
    
    pthread_mutex_unlock(&file->lock);
    return bytes_read;
}

/* Main Function - Demonstration */

int main(void) {
    printf("Initializing File Readahead System...\n");
    
    // Initialize global page cache
    cache_init(&global_cache);
    
    // Create a test file
    const char *test_file = "test_file.dat";
    FILE *fp = fopen(test_file, "w");
    if (!fp) {
        fprintf(stderr, "Failed to create test file\n");
        return 1;
    }
    
    // Write some test data
    printf("Creating test file with 1MB of data...\n");
    char buffer[PAGE_SIZE];
    for (int i = 0; i < 256; i++) {  // 256 pages = 1MB
        memset(buffer, i, PAGE_SIZE);
        fwrite(buffer, 1, PAGE_SIZE, fp);
    }
    fclose(fp);
    
    // Open file with readahead
    struct file_handle *file = ra_open(test_file);
    if (!file) {
        fprintf(stderr, "Failed to open file\n");
        unlink(test_file);
        return 1;
    }
    
    printf("\nPerforming sequential reads...\n");
    
    // Perform sequential reads
    char read_buf[PAGE_SIZE];
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    for (off_t offset = 0; offset < file->size; offset += PAGE_SIZE) {
        ssize_t bytes = ra_read(file, read_buf, PAGE_SIZE, offset);
        if (bytes != PAGE_SIZE) {
            fprintf(stderr, "Read failed at offset %ld\n", offset);
            break;
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long long elapsed = time_diff_ms(&start_time, &end_time);
    
    printf("Sequential read completed in %lld ms\n", elapsed);
    printf("Average throughput: %.2f MB/s\n",
           (file->size / 1024.0 / 1024.0) / (elapsed / 1000.0));
           
    printf("\nPerforming random reads...\n");
    
    // Perform random reads
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    for (int i = 0; i < 100; i++) {
        off_t offset = (rand() % (file->size / PAGE_SIZE)) * PAGE_SIZE;
        ssize_t bytes = ra_read(file, read_buf, PAGE_SIZE, offset);
        if (bytes != PAGE_SIZE) {
            fprintf(stderr, "Read failed at offset %ld\n", offset);
            break;
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed = time_diff_ms(&start_time, &end_time);
    
    printf("Random reads completed in %lld ms\n", elapsed);
    
    // Cleanup
    ra_close(file);
    unlink(test_file);
    
    printf("\nCleaning up...\n");
    
    // Free cache pages
    for (unsigned long i = 0; i < global_cache.nr_pages; i++) {
        if (global_cache.pages[i]) {
            free_page(global_cache.pages[i]);
            global_cache.pages[i] = NULL;
        }
    }
    
    pthread_mutex_destroy(&global_cache.lock);
    pthread_cond_destroy(&global_cache.cond);
    
    return 0;
}
