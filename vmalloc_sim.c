#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Simulated page size
#define PAGE_SIZE 4096
#define VMALLOC_START 0x10000000
#define VMALLOC_END   0x7FFFFFFF

// Red-Black Tree Node Color
typedef enum {
    RB_BLACK,
    RB_RED
} rb_color_t;

// Virtual Memory Area Structure
typedef struct vmap_area {
    unsigned long va_start;   // Start of virtual address
    unsigned long va_end;     // End of virtual address
    void *private_data;       // Optional private data

    // Red-Black Tree Node
    struct vmap_area *parent;
    struct vmap_area *left;
    struct vmap_area *right;
    rb_color_t color;
} vmap_area_t;

// Global Virtual Memory Root
static vmap_area_t *vmalloc_root = NULL;

// Utility Function Prototypes
static void rotate_left(vmap_area_t **root, vmap_area_t *x);
static void rotate_right(vmap_area_t **root, vmap_area_t *x);
static void insert_fixup(vmap_area_t **root, vmap_area_t *z);
static void delete_fixup(vmap_area_t **root, vmap_area_t *x);
static vmap_area_t* find_successor(vmap_area_t *node);
static void transplant(vmap_area_t **root, vmap_area_t *u, vmap_area_t *v);

// Rotate Left in Red-Black Tree
static void rotate_left(vmap_area_t **root, vmap_area_t *x) {
    vmap_area_t *y = x->right;
    x->right = y->left;
    
    if (y->left != NULL) {
        y->left->parent = x;
    }
    
    y->parent = x->parent;
    
    if (x->parent == NULL) {
        *root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    
    y->left = x;
    x->parent = y;
}

// Rotate Right in Red-Black Tree
static void rotate_right(vmap_area_t **root, vmap_area_t *x) {
    vmap_area_t *y = x->left;
    x->left = y->right;
    
    if (y->right != NULL) {
        y->right->parent = x;
    }
    
    y->parent = x->parent;
    
    if (x->parent == NULL) {
        *root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    
    y->right = x;
    x->parent = y;
}

// Fix Red-Black Tree after insertion
static void insert_fixup(vmap_area_t **root, vmap_area_t *z) {
    while (z->parent != NULL && z->parent->color == RB_RED) {
        if (z->parent == z->parent->parent->left) {
            vmap_area_t *y = z->parent->parent->right;
            
            if (y != NULL && y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotate_left(root, z);
                }
                
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_right(root, z->parent->parent);
            }
        } else {
            vmap_area_t *y = z->parent->parent->left;
            
            if (y != NULL && y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(root, z);
                }
                
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_left(root, z->parent->parent);
            }
        }
        
        if (z == *root) {
            break;
        }
    }
    
    (*root)->color = RB_BLACK;
}

// Find successor for deletion
static vmap_area_t* find_successor(vmap_area_t *node) {
    while (node->left != NULL) {
        node = node->left;
    }
    return node;
}

// Transplant nodes during deletion
static void transplant(vmap_area_t **root, vmap_area_t *u, vmap_area_t *v) {
    if (u->parent == NULL) {
        *root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    
    if (v != NULL) {
        v->parent = u->parent;
    }
}

// Fix Red-Black Tree after deletion
static void delete_fixup(vmap_area_t **root, vmap_area_t *x) {
    while (x != *root && x->color == RB_BLACK) {
        if (x == x->parent->left) {
            vmap_area_t *w = x->parent->right;
            
            if (w->color == RB_RED) {
                w->color = RB_BLACK;
                x->parent->color = RB_RED;
                rotate_left(root, x->parent);
                w = x->parent->right;
            }
            
            if ((w->left == NULL || w->left->color == RB_BLACK) &&
                (w->right == NULL || w->right->color == RB_BLACK)) {
                w->color = RB_RED;
                x = x->parent;
            } else {
                if (w->right == NULL || w->right->color == RB_BLACK) {
                    w->left->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_right(root, w);
                    w = x->parent->right;
                }
                
                w->color = x->parent->color;
                x->parent->color = RB_BLACK;
                w->right->color = RB_BLACK;
                rotate_left(root, x->parent);
                x = *root;
            }
        } else {
            // Mirror image of the above case
            vmap_area_t *w = x->parent->left;
            
            if (w->color == RB_RED) {
                w->color = RB_BLACK;
                x->parent->color = RB_RED;
                rotate_right(root, x->parent);
                w = x->parent->left;
            }
            
            if ((w->right == NULL || w->right->color == RB_BLACK) &&
                (w->left == NULL || w->left->color == RB_BLACK)) {
                w->color = RB_RED;
                x = x->parent;
            } else {
                if (w->left == NULL || w->left->color == RB_BLACK) {
                    w->right->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_left(root, w);
                    w = x->parent->left;
                }
                
                w->color = x->parent->color;
                x->parent->color = RB_BLACK;
                w->left->color = RB_BLACK;
                rotate_right(root, x->parent);
                x = *root;
            }
        }
    }
    
    x->color = RB_BLACK;
}

// Memory Allocation Function
void* vmalloc_sim(size_t size) {
    // Align size to page size
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // Find a suitable virtual memory area
    vmap_area_t *va = vmalloc_root;
    vmap_area_t *prev = NULL;
    unsigned long addr = VMALLOC_START;

    while (va != NULL) {
        if (addr + size <= va->va_start) {
            break;
        }
        prev = va;
        addr = va->va_end;
        va = (addr < va->va_start) ? va->left : va->right;
    }

    // Check if we've exceeded the virtual memory range
    if (addr + size > VMALLOC_END) {
        return NULL;
    }

    // Create new virtual memory area
    vmap_area_t *new_va = malloc(sizeof(vmap_area_t));
    if (new_va == NULL) {
        return NULL;
    }

    new_va->va_start = addr;
    new_va->va_end = addr + size;
    new_va->private_data = malloc(size);
    
    if (new_va->private_data == NULL) {
        free(new_va);
        return NULL;
    }

    // Insert into red-black tree
    new_va->parent = prev;
    new_va->left = new_va->right = NULL;
    new_va->color = RB_RED;

    if (prev == NULL) {
        vmalloc_root = new_va;
    } else if (addr < prev->va_start) {
        prev->left = new_va;
    } else {
        prev->right = new_va;
    }

    insert_fixup(&vmalloc_root, new_va);

    return new_va->private_data;
}

// Memory Free Function
void vfree_sim(void *addr) {
    if (addr == NULL) return;

    // Find the corresponding vmap_area
    vmap_area_t *va = vmalloc_root;
    while (va != NULL) {
        if (addr >= va->private_data && 
            addr < (void*)((char*)va->private_data + (va->va_end - va->va_start))) {
            break;
        }
        va = (addr < va->private_data) ? va->left : va->right;
    }

    if (va == NULL) {
        fprintf(stderr, "Invalid address: cannot free\n");
        return;
    }

    // Find node to remove
    vmap_area_t *z = va;
    vmap_area_t *y = z;
    vmap_area_t *x;
    rb_color_t y_original_color = y->color;

    if (z->left == NULL) {
        x = z->right;
        transplant(&vmalloc_root, z, z->right);
    } else if (z->right == NULL) {
        x = z->left;
        transplant(&vmalloc_root, z, z->left);
    } else {
        y = find_successor(z->right);
        y_original_color = y->color;
        x = y->right;

        if (y->parent == z) {
            if (x) x->parent = y;
        } else {
            transplant(&vmalloc_root, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }

        transplant(&vmalloc_root, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    // Free the memory
    free(va->private_data);

    // Fix RB tree if needed
    if (y_original_color == RB_BLACK && x != NULL) {
        delete_fixup(&vmalloc_root, x);
    }

    // Free the vmap_area
    free(va);
}

// Demonstration Function
void demonstrate_vmalloc(void) {
    printf("Virtual Memory Allocation Simulation\n");
    printf("------------------------------------\n");

    // Allocate memory
    int *arr1 = vmalloc_sim(10 * sizeof(int));
    int *arr2 = vmalloc_sim(20 * sizeof(int));

    if (arr1 == NULL || arr2 == NULL) {
        printf("Memory allocation failed\n");
        return;
    }

    // Use the memory
    for (int i = 0; i < 10; i++) {
        arr1[i] = i * 2;
    }

    for (int i = 0; i < 20; i++) {
        arr2[i] = i * 3;
    }

    // Print some values
    printf("arr1[5] = %d\n", arr1[5]);
    printf("arr2[10] = %d\n", arr2[10]);

    // Free memory
    vfree_sim(arr1);
    vfree_sim(arr2);
}

int main(void) {
    demonstrate_vmalloc();
    return 0;
}
