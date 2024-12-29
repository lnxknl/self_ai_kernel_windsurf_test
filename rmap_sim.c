/*
 * Reverse Mapping (RMAP) System Simulation
 * 
 * This program simulates a reverse mapping system similar to the Linux kernel's
 * rmap mechanism. It demonstrates how physical pages can be tracked through their
 * virtual mappings, which is essential for memory management operations like
 * page reclamation and migration.
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

/* Configuration Constants */
#define PAGE_SIZE           4096    // Size of each page in bytes
#define MAX_PAGES          1024    // Maximum number of physical pages
#define MAX_PROCESSES       64     // Maximum number of processes
#define MAX_VMAPS          256    // Maximum number of VMAs per process
#define MAX_ANON_VMAS      512   // Maximum number of anonymous VMAs
#define PAGE_SHIFT         12    // Page size shift (2^12 = 4096)
#define PMD_SHIFT          21   // PMD size shift (2^21 = 2MB)
#define PTE_PER_PAGE      512  // Number of PTEs per page table
#define HUGE_PAGE_SIZE    (1UL << PMD_SHIFT)  // 2MB huge page size

/* Page Flags */
#define PAGE_FLAG_LOCKED    0x0001  // Page is locked
#define PAGE_FLAG_DIRTY     0x0002  // Page is dirty
#define PAGE_FLAG_ACTIVE    0x0004  // Page is in active list
#define PAGE_FLAG_ANON      0x0008  // Page is anonymous
#define PAGE_FLAG_MAPPED    0x0010  // Page is mapped
#define PAGE_FLAG_COMPOUND  0x0020  // Page is part of huge page
#define PAGE_FLAG_HEAD      0x0040  // Page is head of compound page
#define PAGE_FLAG_TAIL      0x0080  // Page is tail of compound page

/* VMA Flags */
#define VM_READ            0x0001  // VMA is readable
#define VM_WRITE           0x0002  // VMA is writable
#define VM_EXEC            0x0004  // VMA is executable
#define VM_SHARED          0x0008  // VMA is shared
#define VM_MAYREAD         0x0010  // VMA may be readable
#define VM_MAYWRITE        0x0020  // VMA may be writable
#define VM_MAYEXEC         0x0040  // VMA may be executable
#define VM_MAYSHARE        0x0080  // VMA may be shared
#define VM_GROWSDOWN       0x0100  // VMA grows downward
#define VM_PFNMAP          0x0200  // VMA has page frame numbers
#define VM_DENYWRITE       0x0400  // VMA denies write access
#define VM_LOCKED          0x0800  // VMA is locked
#define VM_IO              0x1000  // VMA maps device I/O space
#define VM_HUGETLB         0x2000  // VMA uses huge pages

/* Error Codes */
#define RMAP_SUCCESS        0      // Operation succeeded
#define RMAP_ERROR        (-1)     // Generic error
#define RMAP_NOMEM        (-2)     // Out of memory
#define RMAP_NOENT        (-3)     // No such entry
#define RMAP_EXIST        (-4)     // Entry exists
#define RMAP_INVAL        (-5)     // Invalid argument
#define RMAP_AGAIN        (-6)     // Try again
#define RMAP_BUSY         (-7)     // Resource busy

/* Forward Declarations */
struct page;
struct vma;
struct mm_struct;
struct anon_vma;
struct rmap_item;

/* Type Definitions */

// Physical page structure
struct page {
    unsigned long flags;           // Page flags
    int _mapcount;                // Count of mappings
    int _refcount;               // Reference count
    struct list_head lru;       // LRU list entry
    union {
        struct {
            struct anon_vma *anon_vma;  // Anonymous VMA
            unsigned long index;         // Page index
        } anon;
        struct {
            unsigned long index;         // File offset >> PAGE_SHIFT
            struct address_space *mapping;  // Associated address space
        } file;
    };
    struct list_head rmap_head;   // List of reverse mappings
    pthread_mutex_t lock;         // Page lock
};

// Virtual memory area structure
struct vma {
    unsigned long vm_start;        // Start address
    unsigned long vm_end;          // End address
    unsigned long vm_flags;        // VMA flags
    struct mm_struct *vm_mm;       // Associated mm_struct
    struct anon_vma *anon_vma;    // Anonymous VMA (if any)
    pgoff_t vm_pgoff;             // Offset in file or anonymous area
    struct list_head vma_list;    // List of VMAs
    struct rb_node vm_rb;         // Red-black tree node
    atomic_t vm_refcount;         // Reference count
};

// Memory descriptor structure
struct mm_struct {
    struct vma *mmap;             // List of VMAs
    struct rb_root mm_rb;         // Red-black tree of VMAs
    unsigned long total_vm;       // Total pages mapped
    unsigned long data_vm;        // Pages in data segments
    unsigned long stack_vm;       // Pages in stack segments
    unsigned long start_code;     // Start address of code
    unsigned long end_code;       // End address of code
    unsigned long start_data;     // Start address of data
    unsigned long end_data;       // End address of data
    unsigned long start_brk;      // Start address of heap
    unsigned long brk;            // End address of heap
    unsigned long start_stack;    // Start address of stack
    pthread_mutex_t mmap_lock;    // Protects VMA list
    int mm_users;                // Number of users
    int mm_count;               // Reference count
};

// Anonymous VMA structure
struct anon_vma {
    struct anon_vma *root;        // Root anon_vma
    atomic_t refcount;            // Reference count
    unsigned degree;              // Degree in tree
    struct rb_root rb_root;       // Red-black tree root
    struct list_head head;        // List head
    pthread_mutex_t lock;         // anon_vma lock
};

// Reverse mapping item structure
struct rmap_item {
    unsigned long address;         // Virtual address
    struct page *page;            // Associated page
    struct vma *vma;              // Associated VMA
    struct list_head anon_list;   // List for anonymous pages
    struct list_head file_list;   // List for file-backed pages
};

/* Global Variables */
static struct page *pages[MAX_PAGES];
static struct mm_struct *processes[MAX_PROCESSES];
static int nr_processes = 0;
static pthread_mutex_t rmap_lock = PTHREAD_MUTEX_INITIALIZER;

/* List Management Functions */

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

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

enum rb_color {
    RB_RED = 0,
    RB_BLACK = 1
};

struct rb_node {
    unsigned long rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
    struct rb_node *rb_node;
};

#define rb_parent(r) ((struct rb_node *)((r)->rb_parent_color & ~3))
#define rb_color(r) ((r)->rb_parent_color & 1)
#define rb_is_red(r) (!rb_color(r))
#define rb_is_black(r) rb_color(r)
#define rb_set_red(r) do { (r)->rb_parent_color &= ~1; } while (0)
#define rb_set_black(r) do { (r)->rb_parent_color |= 1; } while (0)

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                              struct rb_node **rb_link) {
    node->rb_parent_color = (unsigned long)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

/* Page Management Functions */

// Initialize a page structure
static void page_init(struct page *page) {
    memset(page, 0, sizeof(*page));
    page->flags = 0;
    page->_mapcount = 0;
    page->_refcount = 1;
    list_init(&page->rmap_head);
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

/* VMA Management Functions */

// Create a new VMA
static struct vma *vma_create(unsigned long start, unsigned long end,
                            unsigned long flags) {
    struct vma *vma = malloc(sizeof(*vma));
    if (!vma)
        return NULL;
        
    vma->vm_start = start;
    vma->vm_end = end;
    vma->vm_flags = flags;
    vma->vm_mm = NULL;
    vma->anon_vma = NULL;
    vma->vm_pgoff = 0;
    list_init(&vma->vma_list);
    atomic_set(&vma->vm_refcount, 1);
    
    return vma;
}

// Destroy a VMA
static void vma_destroy(struct vma *vma) {
    if (!vma)
        return;
    if (vma->anon_vma)
        anon_vma_put(vma->anon_vma);
    free(vma);
}

/* Anonymous VMA Management */

// Create an anonymous VMA
static struct anon_vma *anon_vma_create(void) {
    struct anon_vma *anon_vma = malloc(sizeof(*anon_vma));
    if (!anon_vma)
        return NULL;
        
    anon_vma->root = anon_vma;
    atomic_set(&anon_vma->refcount, 1);
    anon_vma->degree = 0;
    anon_vma->rb_root.rb_node = NULL;
    list_init(&anon_vma->head);
    pthread_mutex_init(&anon_vma->lock, NULL);
    
    return anon_vma;
}

// Destroy an anonymous VMA
static void anon_vma_destroy(struct anon_vma *anon_vma) {
    if (!anon_vma)
        return;
    pthread_mutex_destroy(&anon_vma->lock);
    free(anon_vma);
}

// Get a reference to an anonymous VMA
static void anon_vma_get(struct anon_vma *anon_vma) {
    atomic_inc(&anon_vma->refcount);
}

// Put a reference to an anonymous VMA
static void anon_vma_put(struct anon_vma *anon_vma) {
    if (atomic_dec_and_test(&anon_vma->refcount))
        anon_vma_destroy(anon_vma);
}

/* Reverse Mapping Functions */

// Create a reverse mapping item
static struct rmap_item *rmap_item_create(struct page *page,
                                        struct vma *vma,
                                        unsigned long address) {
    struct rmap_item *rmap = malloc(sizeof(*rmap));
    if (!rmap)
        return NULL;
        
    rmap->page = page;
    rmap->vma = vma;
    rmap->address = address;
    list_init(&rmap->anon_list);
    list_init(&rmap->file_list);
    
    return rmap;
}

// Add a reverse mapping
static int page_add_rmap(struct page *page, struct vma *vma,
                        unsigned long address) {
    struct rmap_item *rmap;
    int ret = RMAP_SUCCESS;
    
    lock_page(page);
    
    // Check if the page is already mapped
    if (page->flags & PAGE_FLAG_MAPPED) {
        ret = RMAP_EXIST;
        goto out;
    }
    
    rmap = rmap_item_create(page, vma, address);
    if (!rmap) {
        ret = RMAP_NOMEM;
        goto out;
    }
    
    // Add to appropriate list based on page type
    if (page->flags & PAGE_FLAG_ANON)
        list_add(&rmap->anon_list, &page->rmap_head);
    else
        list_add(&rmap->file_list, &page->rmap_head);
        
    page->flags |= PAGE_FLAG_MAPPED;
    page->_mapcount++;
    
out:
    unlock_page(page);
    return ret;
}

// Remove a reverse mapping
static int page_remove_rmap(struct page *page, struct vma *vma,
                          unsigned long address) {
    struct rmap_item *rmap, *tmp;
    struct list_head *head;
    int ret = RMAP_NOENT;
    
    lock_page(page);
    
    // Choose appropriate list based on page type
    head = (page->flags & PAGE_FLAG_ANON) ?
           &page->rmap_head : &page->rmap_head;
           
    // Find and remove the mapping
    list_for_each_entry_safe(rmap, tmp, head, anon_list) {
        if (rmap->vma == vma && rmap->address == address) {
            list_del(&rmap->anon_list);
            free(rmap);
            page->_mapcount--;
            if (page->_mapcount == 0)
                page->flags &= ~PAGE_FLAG_MAPPED;
            ret = RMAP_SUCCESS;
            break;
        }
    }
    
    unlock_page(page);
    return ret;
}

// Try to unmap all PTEs mapping a page
static int try_to_unmap(struct page *page) {
    struct rmap_item *rmap, *tmp;
    struct list_head *head;
    int ret = RMAP_SUCCESS;
    
    lock_page(page);
    
    if (!(page->flags & PAGE_FLAG_MAPPED)) {
        ret = RMAP_NOENT;
        goto out;
    }
    
    head = (page->flags & PAGE_FLAG_ANON) ?
           &page->rmap_head : &page->rmap_head;
           
    list_for_each_entry_safe(rmap, tmp, head, anon_list) {
        if (page_remove_rmap(page, rmap->vma, rmap->address) != RMAP_SUCCESS) {
            ret = RMAP_ERROR;
            goto out;
        }
    }
    
out:
    unlock_page(page);
    return ret;
}

/* Memory Management Functions */

// Initialize the memory management system
static int mm_init(void) {
    int i;
    
    // Initialize global pages array
    for (i = 0; i < MAX_PAGES; i++) {
        pages[i] = alloc_page();
        if (!pages[i])
            goto cleanup;
    }
    
    // Initialize process array
    memset(processes, 0, sizeof(processes));
    nr_processes = 0;
    
    return RMAP_SUCCESS;
    
cleanup:
    while (--i >= 0)
        free_page(pages[i]);
    return RMAP_NOMEM;
}

// Cleanup the memory management system
static void mm_cleanup(void) {
    int i;
    
    // Free all pages
    for (i = 0; i < MAX_PAGES; i++) {
        if (pages[i])
            free_page(pages[i]);
    }
    
    // Free all processes
    for (i = 0; i < nr_processes; i++) {
        if (processes[i]) {
            struct vma *vma, *tmp;
            
            // Free all VMAs
            list_for_each_entry_safe(vma, tmp, &processes[i]->mmap->vma_list,
                                   vma_list) {
                vma_destroy(vma);
            }
            
            free(processes[i]);
        }
    }
}

/* Main Function - Demonstration */

int main(void) {
    printf("Initializing Reverse Mapping (RMAP) System...\n");
    
    if (mm_init() != RMAP_SUCCESS) {
        fprintf(stderr, "Failed to initialize memory management system\n");
        return 1;
    }
    
    printf("\nSimulating memory operations...\n");
    
    // Create a test process
    struct mm_struct *mm = malloc(sizeof(*mm));
    if (!mm) {
        fprintf(stderr, "Failed to allocate memory descriptor\n");
        goto cleanup;
    }
    
    memset(mm, 0, sizeof(*mm));
    pthread_mutex_init(&mm->mmap_lock, NULL);
    mm->mm_users = 1;
    mm->mm_count = 1;
    processes[nr_processes++] = mm;
    
    // Create a test VMA
    struct vma *vma = vma_create(0x1000, 0x2000, VM_READ | VM_WRITE);
    if (!vma) {
        fprintf(stderr, "Failed to create VMA\n");
        goto cleanup;
    }
    
    vma->vm_mm = mm;
    list_add(&vma->vma_list, &mm->mmap->vma_list);
    
    // Create an anonymous VMA
    struct anon_vma *anon_vma = anon_vma_create();
    if (!anon_vma) {
        fprintf(stderr, "Failed to create anonymous VMA\n");
        goto cleanup;
    }
    
    vma->anon_vma = anon_vma;
    
    // Map some pages
    printf("Mapping pages...\n");
    for (int i = 0; i < 5; i++) {
        unsigned long addr = vma->vm_start + (i * PAGE_SIZE);
        struct page *page = pages[i];
        
        page->flags |= PAGE_FLAG_ANON;
        if (page_add_rmap(page, vma, addr) != RMAP_SUCCESS) {
            fprintf(stderr, "Failed to add reverse mapping for page %d\n", i);
            continue;
        }
        
        printf("Mapped page %d at address 0x%lx\n", i, addr);
    }
    
    // Try to unmap pages
    printf("\nUnmapping pages...\n");
    for (int i = 0; i < 5; i++) {
        struct page *page = pages[i];
        
        if (try_to_unmap(page) != RMAP_SUCCESS) {
            fprintf(stderr, "Failed to unmap page %d\n", i);
            continue;
        }
        
        printf("Unmapped page %d\n", i);
    }
    
    printf("\nCleaning up...\n");
    
cleanup:
    mm_cleanup();
    printf("RMAP simulation completed.\n");
    return 0;
}
