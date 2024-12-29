#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btree_standalone.h"

/* Predefined geometries */
struct btree_geo btree_geo32 = {
    .keylen = 1,
    .no_pairs = (NODESIZE - sizeof(void *)) / (sizeof(unsigned long) + sizeof(void *)),
    .no_longs = (NODESIZE - sizeof(void *)) / sizeof(unsigned long)
};

struct btree_geo btree_geo64 = {
    .keylen = 2,
    .no_pairs = (NODESIZE - sizeof(void *)) / (2 * sizeof(unsigned long) + sizeof(void *)),
    .no_longs = (NODESIZE - sizeof(void *)) / sizeof(unsigned long)
};

struct btree_geo btree_geo128 = {
    .keylen = 4,
    .no_pairs = (NODESIZE - sizeof(void *)) / (4 * sizeof(unsigned long) + sizeof(void *)),
    .no_longs = (NODESIZE - sizeof(void *)) / sizeof(unsigned long)
};

/* Helper functions */
static struct btree_node *create_node(struct btree_geo *geo, bool is_leaf) {
    struct btree_node *node = calloc(1, sizeof(*node));
    if (!node)
        return NULL;

    node->keys = calloc(2 * geo->no_pairs, sizeof(unsigned long));
    node->values = calloc(2 * geo->no_pairs, sizeof(void *));
    if (!is_leaf)
        node->children = calloc(2 * geo->no_pairs + 1, sizeof(struct btree_node *));

    node->is_leaf = is_leaf;
    return node;
}

static void free_node(struct btree_node *node) {
    free(node->keys);
    free(node->values);
    free(node->children);
    free(node);
}

static int compare_keys(unsigned long *k1, unsigned long *k2, int keylen) {
    for (int i = 0; i < keylen; i++) {
        if (k1[i] < k2[i])
            return -1;
        if (k1[i] > k2[i])
            return 1;
    }
    return 0;
}

static void copy_key(unsigned long *dst, unsigned long *src, int keylen) {
    memcpy(dst, src, keylen * sizeof(unsigned long));
}

struct btree_head *btree_init(struct btree_geo *geo) {
    struct btree_head *head = calloc(1, sizeof(*head));
    if (!head)
        return NULL;

    head->root = create_node(geo, true);
    if (!head->root) {
        free(head);
        return NULL;
    }

    head->height = 1;
    head->geo = geo;
    return head;
}

void btree_destroy(struct btree_head *head) {
    if (!head)
        return;

    struct btree_node *stack[32] = {head->root};
    int stack_size = 1;

    while (stack_size > 0) {
        struct btree_node *node = stack[--stack_size];
        if (!node)
            continue;

        if (!node->is_leaf) {
            for (int i = 0; i <= node->count; i++) {
                if (node->children[i])
                    stack[stack_size++] = node->children[i];
            }
        }
        free_node(node);
    }

    free(head);
}

static bool split_child(struct btree_head *head, struct btree_node *parent,
                       int index, struct btree_node *child) {
    struct btree_geo *geo = head->geo;
    struct btree_node *new_child = create_node(geo, child->is_leaf);
    if (!new_child)
        return false;

    /* Move half of the keys to the new node */
    int mid = geo->no_pairs / 2;
    new_child->count = geo->no_pairs - mid - 1;
    
    for (int i = 0; i < new_child->count; i++) {
        copy_key(&new_child->keys[i * geo->keylen],
                &child->keys[(mid + 1 + i) * geo->keylen], geo->keylen);
        new_child->values[i] = child->values[mid + 1 + i];
    }

    if (!child->is_leaf) {
        for (int i = 0; i <= new_child->count; i++)
            new_child->children[i] = child->children[mid + 1 + i];
    }

    child->count = mid;

    /* Move parent keys to make room */
    for (int i = parent->count; i > index; i--) {
        copy_key(&parent->keys[i * geo->keylen],
                &parent->keys[(i - 1) * geo->keylen], geo->keylen);
        parent->values[i] = parent->values[i - 1];
    }

    /* Move parent children to make room */
    if (!parent->is_leaf) {
        for (int i = parent->count + 1; i > index + 1; i--)
            parent->children[i] = parent->children[i - 1];
        parent->children[index + 1] = new_child;
    }

    /* Insert the middle key into the parent */
    copy_key(&parent->keys[index * geo->keylen],
            &child->keys[mid * geo->keylen], geo->keylen);
    parent->values[index] = child->values[mid];
    parent->count++;

    return true;
}

static bool insert_nonfull(struct btree_head *head, struct btree_node *node,
                          unsigned long *key, void *val) {
    struct btree_geo *geo = head->geo;
    int i = node->count - 1;

    if (node->is_leaf) {
        /* Find position and insert */
        while (i >= 0 && compare_keys(&node->keys[i * geo->keylen], key, geo->keylen) > 0) {
            copy_key(&node->keys[(i + 1) * geo->keylen],
                    &node->keys[i * geo->keylen], geo->keylen);
            node->values[i + 1] = node->values[i];
            i--;
        }

        copy_key(&node->keys[(i + 1) * geo->keylen], key, geo->keylen);
        node->values[i + 1] = val;
        node->count++;
        return true;
    }

    /* Find child to recurse */
    while (i >= 0 && compare_keys(&node->keys[i * geo->keylen], key, geo->keylen) > 0)
        i--;
    i++;

    if (node->children[i]->count == 2 * geo->no_pairs - 1) {
        if (!split_child(head, node, i, node->children[i]))
            return false;
        if (compare_keys(&node->keys[i * geo->keylen], key, geo->keylen) < 0)
            i++;
    }

    return insert_nonfull(head, node->children[i], key, val);
}

bool btree_insert(struct btree_head *head, unsigned long *key, void *val) {
    if (!head || !key || !val)
        return false;

    struct btree_node *root = head->root;
    if (root->count == 2 * head->geo->no_pairs - 1) {
        struct btree_node *new_root = create_node(head->geo, false);
        if (!new_root)
            return false;

        head->root = new_root;
        new_root->children[0] = root;
        head->height++;

        if (!split_child(head, new_root, 0, root))
            return false;

        return insert_nonfull(head, new_root, key, val);
    }

    return insert_nonfull(head, root, key, val);
}

void *btree_lookup(struct btree_head *head, unsigned long *key) {
    if (!head || !key)
        return NULL;

    struct btree_node *node = head->root;
    struct btree_geo *geo = head->geo;

    while (node) {
        int i = 0;
        while (i < node->count && 
               compare_keys(&node->keys[i * geo->keylen], key, geo->keylen) < 0)
            i++;

        if (i < node->count && 
            compare_keys(&node->keys[i * geo->keylen], key, geo->keylen) == 0)
            return node->values[i];

        if (node->is_leaf)
            break;

        node = node->children[i];
    }

    return NULL;
}

static void print_node(struct btree_node *node, struct btree_geo *geo, int level) {
    if (!node)
        return;

    for (int i = 0; i < level; i++)
        printf("  ");

    printf("Node(%d): ", node->count);
    for (int i = 0; i < node->count; i++)
        printf("%lu ", node->keys[i * geo->keylen]);
    printf("\n");

    if (!node->is_leaf) {
        for (int i = 0; i <= node->count; i++)
            print_node(node->children[i], geo, level + 1);
    }
}

void btree_print(struct btree_head *head) {
    if (!head)
        return;
    printf("B-tree (height %d):\n", head->height);
    print_node(head->root, head->geo, 0);
}

/* Example usage */
int main(void) {
    struct btree_head *head = btree_init(&btree_geo32);
    if (!head) {
        printf("Failed to initialize B-tree\n");
        return 1;
    }

    /* Insert some test values */
    unsigned long keys[] = {5, 3, 7, 1, 9, 6, 4, 2, 8};
    char *values[] = {"five", "three", "seven", "one", "nine",
                     "six", "four", "two", "eight"};

    printf("Inserting test values...\n");
    for (int i = 0; i < 9; i++) {
        if (!btree_insert(head, &keys[i], values[i])) {
            printf("Failed to insert key %lu\n", keys[i]);
            btree_destroy(head);
            return 1;
        }
    }

    /* Print the tree structure */
    printf("\nTree structure after insertions:\n");
    btree_print(head);

    /* Look up some values */
    printf("\nLooking up values:\n");
    unsigned long test_key = 5;
    char *value = btree_lookup(head, &test_key);
    printf("Key %lu -> %s\n", test_key, value ? value : "not found");

    test_key = 10;
    value = btree_lookup(head, &test_key);
    printf("Key %lu -> %s\n", test_key, value ? value : "not found");

    /* Clean up */
    btree_destroy(head);
    return 0;
}
