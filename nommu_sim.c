/*
 * NoMMU Memory Management Simulation
 * 
 * This program simulates memory management for systems without MMU support,
 * demonstrating how memory can be managed without virtual memory capabilities.
 * It implements direct physical memory mapping, shared memory regions, and
 * basic memory protection.
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
#include <stdatomic.h>

/* Configuration Constants */
#define PAGE_SIZE           4096    // Size of each page in bytes
#define MAX_PAGES          1024    // Maximum number of physical pages
#define MAX_REGIONS        256     // Maximum number of memory regions
#define MAX_PROCESSES      64      // Maximum number of processes
#define PAGE_SHIFT         12      // Page size shift (2^12 = 4096)
#define REGION_MIN_SIZE    4096    // Minimum region size
#define REGION_MAX_SIZE    (256 * 1024 * 1024)  // Maximum region size (256MB)

/* Memory Protection Flags */
#define PROT_NONE       0x0       // Page cannot be accessed
#define PROT_READ       0x1       // Page can be read
#define PROT_WRITE      0x2       // Page can be written
#define PROT_EXEC       0x4       // Page can be executed

/* Memory Mapping Flags */
#define MAP_SHARED      0x01      // Share changes
#define MAP_PRIVATE     0x02      // Changes are private
#define MAP_FIXED       0x04      // Interpret addr exactly
#define MAP_ANONYMOUS   0x08      // Don't use a file
#define MAP_GROWSDOWN   0x10      // Stack-like segment
#define MAP_LOCKED      0x20      // Lock the mapping

/* Error Codes */
#define NOMMU_SUCCESS   0         // Operation succeeded
#define NOMMU_ENOMEM   (-1)      // Out of memory
#define NOMMU_EINVAL   (-2)      // Invalid argument
#define NOMMU_EACCES   (-3)      // Permission denied
#define NOMMU_EAGAIN   (-4)      // Try again
#define NOMMU_ENOMAPPING (-5)    // No mapping exists

/* Forward Declarations */
struct mm_struct;
struct vm_area_struct;
struct vm_region;
struct page;

/* Type Definitions */

// Physical page structure
struct page {
    unsigned long flags;           // Page flags
    atomic_int _refcount;         // Reference count
    void *virtual;                // Virtual address (simulated)
    struct list_head lru;         // LRU list entry
    unsigned long private;        // Private data
    pthread_mutex_t lock;         // Page lock
};

// Memory region structure (represents a contiguous memory region)
struct vm_region {
    unsigned long vm_start;       // Start address
    unsigned long vm_end;         // End address
    unsigned long vm_flags;       // Protection flags
    struct rb_node rb;           // Red-black tree node
    atomic_int count;            // Reference count
    struct list_head vm_list;    // Region list
    struct mm_struct *vm_mm;     // Associated mm_struct
};

// Virtual memory area structure
struct vm_area_struct {
    unsigned long vm_start;       // Start address
    unsigned long vm_end;         // End address
    unsigned long vm_flags;       // VMA flags
    struct vm_region *vm_region; // Associated region
    struct mm_struct *vm_mm;     // Associated mm_struct
    struct list_head vm_list;    // VMA list
};

// Memory descriptor structure
struct mm_struct {
    struct vm_area_struct *mmap;    // List of VMAs
    struct rb_root mm_rb;           // Red-black tree root
    unsigned long total_vm;         // Total pages mapped
    unsigned long locked_vm;        // Pages locked in memory
    atomic_t mm_users;             // User count
    atomic_t mm_count;             // Reference count
    pthread_rwlock_t mmap_lock;     // Protects the mm_struct
    unsigned long start_code;       // Start of code segment
    unsigned long end_code;         // End of code segment
    unsigned long start_data;       // Start of data segment
    unsigned long end_data;         // End of data segment
    unsigned long start_brk;        // Start of heap
    unsigned long brk;              // Current heap end
    unsigned long start_stack;      // Start of stack
};

// Red-black tree node
struct rb_node {
    unsigned long rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

// Red-black tree root
struct rb_root {
    struct rb_node *rb_node;
};

// List management structure
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* Global Variables */
static struct page *pages[MAX_PAGES];
static struct vm_region *regions[MAX_REGIONS];
static struct mm_struct *processes[MAX_PROCESSES];
static struct rb_root nommu_region_tree = {NULL};
static pthread_rwlock_t nommu_region_lock = PTHREAD_RWLOCK_INITIALIZER;
static atomic_long_t total_mapped_pages = ATOMIC_VAR_INIT(0);

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

/* Red-Black Tree Functions */

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                              struct rb_node **rb_link) {
    node->rb_parent_color = (unsigned long)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

static void rb_insert_color(struct rb_node *node, struct rb_root *root) {
    struct rb_node *parent, *gparent;

    while ((parent = (struct rb_node *)((node->rb_parent_color) & ~3)) && 
           (parent->rb_parent_color & 1) == 0) {
        gparent = (struct rb_node *)((parent->rb_parent_color) & ~3);

        if (parent == gparent->rb_left) {
            struct rb_node *uncle = gparent->rb_right;

            if (uncle && (uncle->rb_parent_color & 1) == 0) {
                uncle->rb_parent_color |= 1;
                parent->rb_parent_color |= 1;
                gparent->rb_parent_color &= ~1;
                node = gparent;
                continue;
            }

            if (parent->rb_right == node) {
                struct rb_node *tmp;
                node = parent;
                tmp = node->rb_right;
                node->rb_right = tmp->rb_left;
                tmp->rb_left = node;
                parent = tmp;
            }

            parent->rb_parent_color |= 1;
            gparent->rb_parent_color &= ~1;
        } else {
            struct rb_node *uncle = gparent->rb_left;

            if (uncle && (uncle->rb_parent_color & 1) == 0) {
                uncle->rb_parent_color |= 1;
                parent->rb_parent_color |= 1;
                gparent->rb_parent_color &= ~1;
                node = gparent;
                continue;
            }

            if (parent->rb_left == node) {
                struct rb_node *tmp;
                node = parent;
                tmp = node->rb_left;
                node->rb_left = tmp->rb_right;
                tmp->rb_right = node;
                parent = tmp;
            }

            parent->rb_parent_color |= 1;
            gparent->rb_parent_color &= ~1;
        }
    }

    root->rb_node->rb_parent_color |= 1;
}

/* Page Management Functions */

// Initialize a page structure
static void page_init(struct page *page) {
    memset(page, 0, sizeof(*page));
    atomic_init(&page->_refcount, 1);
    pthread_mutex_init(&page->lock, NULL);
    list_init(&page->lru);
}

// Allocate a new page
static struct page *alloc_page(void) {
    struct page *page = malloc(sizeof(struct page));
    if (!page)
        return NULL;
    page_init(page);
    page->virtual = malloc(PAGE_SIZE);
    if (!page->virtual) {
        free(page);
        return NULL;
    }
    return page;
}

// Free a page
static void free_page(struct page *page) {
    if (!page)
        return;
    if (page->virtual)
        free(page->virtual);
    pthread_mutex_destroy(&page->lock);
    free(page);
}

// Lock a page
static void lock_page(struct page *page) {
    pthread_mutex_lock(&page->lock);
}

// Unlock a page
static void unlock_page(struct page *page) {
    pthread_mutex_unlock(&page->lock);
}

/* Memory Region Management */

// Create a new memory region
static struct vm_region *create_vm_region(unsigned long start,
                                        unsigned long end,
                                        unsigned long flags) {
    struct vm_region *region = malloc(sizeof(*region));
    if (!region)
        return NULL;
        
    region->vm_start = start;
    region->vm_end = end;
    region->vm_flags = flags;
    atomic_init(&region->count, 1);
    list_init(&region->vm_list);
    
    return region;
}

// Add a region to the global tree
static void add_region_to_tree(struct vm_region *region) {
    struct rb_node **p = &nommu_region_tree.rb_node;
    struct rb_node *parent = NULL;
    struct vm_region *tmp;

    pthread_rwlock_wrlock(&nommu_region_lock);

    while (*p) {
        parent = *p;
        tmp = rb_entry(parent, struct vm_region, rb);

        if (region->vm_start < tmp->vm_start)
            p = &(*p)->rb_left;
        else if (region->vm_start > tmp->vm_start)
            p = &(*p)->rb_right;
        else
            goto out;
    }

    rb_link_node(&region->rb, parent, p);
    rb_insert_color(&region->rb, &nommu_region_tree);

out:
    pthread_rwlock_unlock(&nommu_region_lock);
}

// Find a region in the global tree
static struct vm_region *find_region(unsigned long addr) {
    struct rb_node *n = nommu_region_tree.rb_node;
    struct vm_region *region;

    pthread_rwlock_rdlock(&nommu_region_lock);

    while (n) {
        region = rb_entry(n, struct vm_region, rb);

        if (addr < region->vm_start)
            n = n->rb_left;
        else if (addr >= region->vm_end)
            n = n->rb_right;
        else {
            pthread_rwlock_unlock(&nommu_region_lock);
            return region;
        }
    }

    pthread_rwlock_unlock(&nommu_region_lock);
    return NULL;
}

/* Memory Mapping Functions */

// Map a memory region
static int do_mmap(struct mm_struct *mm,
                  unsigned long addr,
                  unsigned long len,
                  unsigned long prot,
                  unsigned long flags) {
    struct vm_region *region;
    struct vm_area_struct *vma;
    unsigned long size;

    // Validate parameters
    if (len == 0 || len > REGION_MAX_SIZE)
        return NOMMU_EINVAL;

    // Align size to page boundary
    size = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // Create new region
    region = create_vm_region(addr, addr + size, prot);
    if (!region)
        return NOMMU_ENOMEM;

    // Create VMA
    vma = malloc(sizeof(*vma));
    if (!vma) {
        free(region);
        return NOMMU_ENOMEM;
    }

    // Initialize VMA
    vma->vm_start = addr;
    vma->vm_end = addr + size;
    vma->vm_flags = flags;
    vma->vm_region = region;
    vma->vm_mm = mm;
    list_init(&vma->vm_list);

    // Add region to tree
    add_region_to_tree(region);

    // Add VMA to mm_struct
    pthread_rwlock_wrlock(&mm->mmap_lock);
    list_add(&vma->vm_list, &mm->mmap->vm_list);
    mm->total_vm += size >> PAGE_SHIFT;
    pthread_rwlock_unlock(&mm->mmap_lock);

    return NOMMU_SUCCESS;
}

// Unmap a memory region
static int do_munmap(struct mm_struct *mm,
                    unsigned long addr,
                    size_t len) {
    struct vm_area_struct *vma;
    struct list_head *ptr;
    unsigned long end = addr + len;

    pthread_rwlock_wrlock(&mm->mmap_lock);

    // Find VMA containing the region
    list_for_each(ptr, &mm->mmap->vm_list) {
        vma = list_entry(ptr, struct vm_area_struct, vm_list);
        if (vma->vm_start <= addr && vma->vm_end >= end) {
            // Remove VMA from list
            list_del(&vma->vm_list);
            mm->total_vm -= (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
            
            // Clean up
            if (atomic_fetch_sub(&vma->vm_region->count, 1) == 1) {
                pthread_rwlock_wrlock(&nommu_region_lock);
                rb_erase(&vma->vm_region->rb, &nommu_region_tree);
                pthread_rwlock_unlock(&nommu_region_lock);
                free(vma->vm_region);
            }
            free(vma);
            
            pthread_rwlock_unlock(&mm->mmap_lock);
            return NOMMU_SUCCESS;
        }
    }

    pthread_rwlock_unlock(&mm->mmap_lock);
    return NOMMU_ENOMAPPING;
}

/* Process Memory Management */

// Initialize mm_struct for a new process
static struct mm_struct *mm_init(void) {
    struct mm_struct *mm = malloc(sizeof(*mm));
    if (!mm)
        return NULL;

    memset(mm, 0, sizeof(*mm));
    atomic_init(&mm->mm_users, 1);
    atomic_init(&mm->mm_count, 1);
    pthread_rwlock_init(&mm->mmap_lock, NULL);
    mm->mmap = malloc(sizeof(struct vm_area_struct));
    if (!mm->mmap) {
        free(mm);
        return NULL;
    }
    list_init(&mm->mmap->vm_list);
    mm->mm_rb.rb_node = NULL;

    return mm;
}

// Clean up mm_struct
static void mm_cleanup(struct mm_struct *mm) {
    struct vm_area_struct *vma;
    struct list_head *ptr, *next;

    if (!mm)
        return;

    // Clean up all VMAs
    list_for_each_safe(ptr, next, &mm->mmap->vm_list) {
        vma = list_entry(ptr, struct vm_area_struct, vm_list);
        list_del(&vma->vm_list);
        if (atomic_fetch_sub(&vma->vm_region->count, 1) == 1) {
            pthread_rwlock_wrlock(&nommu_region_lock);
            rb_erase(&vma->vm_region->rb, &nommu_region_tree);
            pthread_rwlock_unlock(&nommu_region_lock);
            free(vma->vm_region);
        }
        free(vma);
    }

    free(mm->mmap);
    pthread_rwlock_destroy(&mm->mmap_lock);
    free(mm);
}

/* System Initialization and Cleanup */

// Initialize the NoMMU memory management system
static int nommu_init(void) {
    int i;

    // Initialize pages
    for (i = 0; i < MAX_PAGES; i++) {
        pages[i] = alloc_page();
        if (!pages[i])
            goto cleanup_pages;
    }

    // Initialize process array
    memset(processes, 0, sizeof(processes));

    return NOMMU_SUCCESS;

cleanup_pages:
    while (--i >= 0)
        free_page(pages[i]);
    return NOMMU_ENOMEM;
}

// Clean up the NoMMU memory management system
static void nommu_cleanup(void) {
    int i;

    // Clean up processes
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i])
            mm_cleanup(processes[i]);
    }

    // Clean up pages
    for (i = 0; i < MAX_PAGES; i++)
        free_page(pages[i]);
}

/* Simulation Functions */

// Simulate memory allocation and mapping
static void simulate_memory_operations(void) {
    struct mm_struct *mm;
    int ret;

    printf("Starting NoMMU memory management simulation...\n\n");

    // Create process memory descriptor
    mm = mm_init();
    if (!mm) {
        printf("Failed to initialize memory descriptor\n");
        return;
    }
    processes[0] = mm;

    printf("1. Mapping anonymous memory regions:\n");

    // Map some memory regions
    ret = do_mmap(mm, 0x1000, 8192, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS);
    printf("- Mapped region 1 (8KB): %s\n",
           ret == NOMMU_SUCCESS ? "SUCCESS" : "FAILED");

    ret = do_mmap(mm, 0x4000, 16384, PROT_READ,
                  MAP_PRIVATE | MAP_ANONYMOUS);
    printf("- Mapped region 2 (16KB): %s\n",
           ret == NOMMU_SUCCESS ? "SUCCESS" : "FAILED");

    printf("\n2. Memory statistics:\n");
    printf("- Total mapped pages: %ld\n", mm->total_vm);
    printf("- Locked pages: %ld\n", mm->locked_vm);

    printf("\n3. Unmapping regions:\n");
    ret = do_munmap(mm, 0x1000, 8192);
    printf("- Unmapped region 1: %s\n",
           ret == NOMMU_SUCCESS ? "SUCCESS" : "FAILED");

    printf("\n4. Updated memory statistics:\n");
    printf("- Total mapped pages: %ld\n", mm->total_vm);
    printf("- Locked pages: %ld\n", mm->locked_vm);
}

/* Main Function */

int main(void) {
    printf("NoMMU Memory Management Simulation\n");
    printf("==================================\n");
    printf("Configuration:\n");
    printf("- Page Size: %d bytes\n", PAGE_SIZE);
    printf("- Max Pages: %d\n", MAX_PAGES);
    printf("- Max Regions: %d\n", MAX_REGIONS);
    printf("- Max Processes: %d\n", MAX_PROCESSES);
    printf("\n");

    if (nommu_init() != NOMMU_SUCCESS) {
        fprintf(stderr, "Failed to initialize NoMMU system\n");
        return 1;
    }

    // Run simulation
    simulate_memory_operations();

    // Cleanup
    nommu_cleanup();

    printf("\nSimulation completed.\n");
    return 0;
}
