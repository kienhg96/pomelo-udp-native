#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include "heap.h"
#include "clz.h"

#ifndef NDEBUG // Debug mode

#define POMELO_HEAP_SIGNATURE 0xe5579f

// Signature generator for debugging
static int heap_signature_generator = 0;

#define pomelo_heap_check_signature(heap)                                      \
    assert((heap)->signature == POMELO_HEAP_SIGNATURE)
#define pomelo_heap_node_check_signature(heap, node)                           \
    assert((node)->signature == (heap)->node_signature)

#else // Release mode
#define pomelo_heap_check_signature(heap)
#define pomelo_heap_node_check_signature(heap, node)
#endif


/// @brief Initialize the entry
static int entry_init(pomelo_heap_entry_t * entry, void * context) {
    (void) context;
    entry->element = (entry + 1);
    return 0;
}


/// @brief Swap two entries
#define swap_entries(node_1, node_2)                                           \
    do {                                                                       \
        pomelo_heap_entry_t * entry_1 = (node_1)->entry;                       \
        pomelo_heap_entry_t * entry_2 = (node_2)->entry;                       \
        (node_1)->entry = entry_2;                                             \
        (node_2)->entry = entry_1;                                             \
        entry_1->node = (node_2);                                              \
        entry_2->node = (node_1);                                              \
    } while (0)


/* -------------------------------------------------------------------------- */
/*                               Public APIs                                  */
/* -------------------------------------------------------------------------- */


pomelo_heap_t * pomelo_heap_create(pomelo_heap_options_t * options) {
    assert(options != NULL);
    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    pomelo_heap_t * heap = pomelo_allocator_malloc_t(allocator, pomelo_heap_t);
    if (!heap) return NULL; // Failed to allocate memory
    memset(heap, 0, sizeof(pomelo_heap_t));

    heap->allocator = allocator;
    heap->element_size = options->element_size;
    heap->compare = options->compare;
    heap->root = NULL;
    heap->size = 0;

#ifndef NDEBUG
    heap->signature = POMELO_HEAP_SIGNATURE;
    heap->node_signature = heap_signature_generator++;
#endif

    // Create the node pool
    pomelo_pool_root_options_t pool_options;
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_heap_node_t);

    heap->node_pool = pomelo_pool_root_create(&pool_options);
    if (!heap->node_pool) {
        pomelo_heap_destroy(heap);
        return NULL; // Failed to create node pool
    }

    // Create the element pool
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size =
        options->element_size + sizeof(pomelo_heap_entry_t);
    pool_options.on_alloc = (pomelo_pool_alloc_cb) entry_init;
    heap->entry_pool = pomelo_pool_root_create(&pool_options);
    if (!heap->entry_pool) {
        pomelo_heap_destroy(heap);
        return NULL; // Failed to create element pool
    }

    if (options->synchronized) {
        heap->mutex = pomelo_mutex_create(allocator);
        if (!heap->mutex) {
            pomelo_heap_destroy(heap);
            return NULL; // Failed to create mutex
        }
    }

    return heap;
}


void pomelo_heap_destroy(pomelo_heap_t * heap) {
    assert(heap != NULL);
    pomelo_heap_check_signature(heap);

    if (heap->node_pool) {
        pomelo_pool_destroy(heap->node_pool);
        heap->node_pool = NULL;
    }

    if (heap->entry_pool) {
        pomelo_pool_destroy(heap->entry_pool);
        heap->entry_pool = NULL;
    }

    if (heap->mutex) {
        pomelo_mutex_destroy(heap->mutex);
        heap->mutex = NULL;
    }

    pomelo_allocator_free(heap->allocator, heap);
}


pomelo_heap_entry_t * pomelo_heap_push_ptr(
    pomelo_heap_t * heap,
    void * p_element
) {
    assert(heap != NULL);
    assert(p_element != NULL);
    pomelo_heap_check_signature(heap);

    pomelo_mutex_t * mutex = heap->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    pomelo_heap_node_t * node = pomelo_pool_acquire(heap->node_pool, NULL);
    if (!node) {
        POMELO_END_CRITICAL_SECTION(mutex);
        return NULL; // Failed to acquire node
    }

    pomelo_heap_entry_t * entry = pomelo_pool_acquire(heap->entry_pool, NULL);
    if (!entry) {
        pomelo_pool_release(heap->node_pool, node);
        POMELO_END_CRITICAL_SECTION(mutex);
        return NULL; // Failed to acquire element
    }
    node->entry = entry;
    entry->node = node;
    memcpy(entry->element, p_element, heap->element_size);

    // Initialize the node link
    node->parent = NULL;
    node->left = NULL;
    node->right = NULL;
    node->next = NULL;

#ifndef NDEBUG
    node->signature = heap->node_signature;
#endif

    if (heap->root == NULL) {
        heap->root = node;
    } else {
        pomelo_heap_node_t * parent = pomelo_heap_find_insert_position(heap);
        assert(parent != NULL);

        if (parent->left == NULL) {
            parent->left = node;
        } else {
            parent->right = node;
        }
        node->parent = parent;
        pomelo_heap_heapify_up(heap, node);
    }

    heap->size++;
    POMELO_END_CRITICAL_SECTION(mutex);
    return entry;
}


int pomelo_heap_pop(pomelo_heap_t * heap, void * p_element) {
    assert(heap != NULL);
    pomelo_heap_check_signature(heap);

    pomelo_mutex_t * mutex = heap->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    if (heap->size == 0) {
        POMELO_END_CRITICAL_SECTION(mutex);
        return -1; // The heap is empty
    }

    pomelo_heap_node_t * root = heap->root;
    if (p_element) {
        memcpy(p_element, root->entry->element, heap->element_size);
    }

    pomelo_heap_remove_node(heap, root);
    POMELO_END_CRITICAL_SECTION(mutex);
    return 0;
}


int pomelo_heap_top(pomelo_heap_t * heap, void * p_element) {
    assert(heap != NULL);
    assert(p_element != NULL);
    pomelo_heap_check_signature(heap);

    pomelo_mutex_t * mutex = heap->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    if (heap->size == 0) {
        POMELO_END_CRITICAL_SECTION(mutex);
        return -1; // The heap is empty
    }
    
    memcpy(p_element, heap->root->entry->element, heap->element_size);
    POMELO_END_CRITICAL_SECTION(mutex);
    return 0;
}


size_t pomelo_heap_size(pomelo_heap_t * heap) {
    assert(heap != NULL);
    pomelo_heap_check_signature(heap);
    pomelo_mutex_t * mutex = heap->mutex;

    POMELO_BEGIN_CRITICAL_SECTION(mutex);
    size_t size = heap->size;
    POMELO_END_CRITICAL_SECTION(mutex);

    return size;
}


void pomelo_heap_remove(pomelo_heap_t * heap, pomelo_heap_entry_t * entry) {
    assert(entry != NULL);
    pomelo_mutex_t * mutex = heap->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);
    
    pomelo_heap_remove_node(heap, entry->node);

    POMELO_END_CRITICAL_SECTION(mutex);
}


void pomelo_heap_clear(pomelo_heap_t * heap) {
    assert(heap != NULL);
    pomelo_heap_check_signature(heap);

    pomelo_mutex_t * mutex = heap->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    if (heap->size == 0) {
        POMELO_END_CRITICAL_SECTION(mutex);
        return; // The heap is empty
    }

    pomelo_heap_node_t * queue = heap->root;
    while (queue != NULL) {
        // Get a node from queue
        pomelo_heap_node_t * current = queue;
        queue = queue->next;
        current->next = NULL;

        // Add left child to queue
        if (current->left) {
            current->left->next = queue;
            queue = current->left;
        }

        // Add right child to queue
        if (current->right) {
            current->right->next = queue;
            queue = current->right;
        }

        #ifndef NDEBUG
            current->signature = 0;
        #endif
        
        // Release the node
        pomelo_pool_release(heap->entry_pool, current->entry);
        current->entry = NULL;

        pomelo_pool_release(heap->node_pool, current);
    }

    heap->root = NULL;
    heap->size = 0;
    POMELO_END_CRITICAL_SECTION(mutex);
}


/* -------------------------------------------------------------------------- */
/*                              Internal APIs                                 */
/* -------------------------------------------------------------------------- */


pomelo_heap_node_t * pomelo_heap_find_insert_position(pomelo_heap_t * heap) {
    assert(heap != NULL);
    assert(heap->root != NULL);

    // Start from root and traverse down to find first available position
    pomelo_heap_node_t * current = heap->root;
    pomelo_heap_node_t * parent = NULL;

    // Use binary representation of size+1 to find insert position
    // Skip first 1 bit since we know root exists
    size_t pos = heap->size + 1;
    uint64_t mask = pomelo_clz_64_mask(pos);
    mask >>= 1; // Move to the bit before most significant

    while (mask > 0) {
        parent = current;
        if (pos & mask) {
            current = current->right;
        } else {
            current = current->left;
        }
        mask >>= 1;
    }

    return parent;
}


pomelo_heap_node_t * pomelo_heap_find_last_node(pomelo_heap_t * heap) {
    assert(heap != NULL);
    assert(heap->root != NULL);

    // Use binary representation of size to find last node
    pomelo_heap_node_t * current = heap->root;
    size_t pos = heap->size;
    uint64_t mask = pomelo_clz_64_mask(pos);
    mask >>= 1;

    while (mask > 0) {
        if (pos & mask) {
            current = current->right;
        } else {
            current = current->left;
        }
        mask >>= 1;
    }

    return current;
}


void pomelo_heap_heapify_up(pomelo_heap_t * heap, pomelo_heap_node_t * node) {
    assert(heap != NULL);
    assert(node != NULL);

    pomelo_heap_node_t * current = node;
    pomelo_heap_compare_fn compare = heap->compare;

    while (current->parent != NULL) {
        pomelo_heap_node_t * parent = current->parent;
        if (compare(parent->entry->element, current->entry->element) < 0) {
            break;
        }

        // Swap the elements
        swap_entries(current, parent);
        current = parent;
    }
}


void pomelo_heap_heapify_down(pomelo_heap_t * heap, pomelo_heap_node_t * node) {
    assert(heap != NULL);
    assert(node != NULL);

    pomelo_heap_node_t * current = node;
    pomelo_heap_compare_fn compare = heap->compare;

    while (true) {
        pomelo_heap_node_t * choosen = current;
        pomelo_heap_node_t * left = current->left;
        pomelo_heap_node_t * right = current->right;
        
        // Check if left child is smaller
        if (left &&
            compare(left->entry->element, choosen->entry->element) < 0
        ) {
            choosen = left;
        }

        // Check if right child is smaller
        if (right &&
            compare(right->entry->element, choosen->entry->element) < 0
        ) {
            choosen = right;
        }

        // If current is already the smallest, we're done
        if (choosen == current) {
            break;
        }

        // Swap elements with the smallest child
        swap_entries(current, choosen);
        current = choosen;
    }
}


void pomelo_heap_remove_node(pomelo_heap_t * heap, pomelo_heap_node_t * node) {
    assert(heap != NULL);
    assert(node != NULL);
    pomelo_heap_check_signature(heap);
    pomelo_heap_node_check_signature(heap, node);

    if (heap->size == 1) {
        assert(node == heap->root);
        pomelo_pool_release(heap->entry_pool, node->entry);
        node->entry = NULL;

        pomelo_pool_release(heap->node_pool, node);
        heap->root = NULL;
        heap->size = 0;
        return;
    }
    
    // Find the last node
    pomelo_heap_node_t * last = pomelo_heap_find_last_node(heap);
    assert(last != NULL);

    // Swap the node and the last node
    swap_entries(node, last);

    // Detach the last node from its parent
    pomelo_heap_node_t * parent = last->parent;
    if (parent->left == last) {
        parent->left = NULL;
    } else {
        parent->right = NULL;
    }

#ifndef NDEBUG
    last->signature = 0;
#endif

    pomelo_pool_release(heap->entry_pool, last->entry);
    last->entry = NULL;

    pomelo_pool_release(heap->node_pool, last);
    heap->size--;

    // Heapify down the root to maintain the heap property
    pomelo_heap_heapify_down(heap, node);
    return;
}
