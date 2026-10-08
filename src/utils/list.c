#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "list.h"
#include "macro.h"


#ifndef NDEBUG // Debug mode

#define POMELO_LIST_SIGNATURE 0x27e241

// Signature generator for debugging
static int entry_signature_generator = 0;

#define pomelo_list_check_signature(list)                                      \
    assert((list)->signature == POMELO_LIST_SIGNATURE)

#define pomelo_list_entry_check_signature(list, entry)                          \
    assert((entry)->signature == (list)->entry_signature)

#else // Release mode

#define pomelo_list_check_signature(list)
#define pomelo_list_entry_check_signature(list, entry)

#endif


/// @brief Get element address from node
#define pomelo_list_element_ptr(entry) ((void *)((entry) + 1))


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Link an entry to the back of the list
static void pomelo_list_link_back(
    pomelo_list_t * list,
    pomelo_list_entry_t * entry
) {
    if (list->size == 0) {
        // Empty
        list->front = entry;
        list->back = entry;
        entry->next = NULL;
        entry->prev = NULL;
    } else {
        entry->prev = list->back;
        entry->next = NULL;
        list->back->next = entry;
        list->back = entry;
    }

    list->size++;
    list->mod_count++;
}


/// @brief Append new entry to back of the list
static pomelo_list_entry_t * pomelo_list_append_back(pomelo_list_t * list) {
    pomelo_list_entry_t * entry =
        pomelo_pool_acquire(list->context->entry_pool, NULL);
    if (!entry) return NULL;

#ifndef NDEBUG
    entry->signature = list->entry_signature;
#endif

    pomelo_list_link_back(list, entry);
    return entry;
}


/// @brief Append new entry to front of the list
static pomelo_list_entry_t * pomelo_list_append_front(pomelo_list_t * list) {
    pomelo_list_entry_t * entry =
        pomelo_pool_acquire(list->context->entry_pool, NULL);
    if (!entry) return NULL;

#ifndef NDEBUG
    entry->signature = list->entry_signature;
#endif

    if (list->size == 0) {
        // Empty
        list->front = entry;
        list->back = entry;
        entry->next = NULL;
        entry->prev = NULL;
    } else {
        entry->next = list->front;
        entry->prev = NULL;
        list->front->prev = entry;
        list->front = entry;
    }

    list->size++;
    list->mod_count++;
    return entry;
}


/// @brief Just unlink the entry from list without releasing it
static void pomelo_list_unlink_entry(
    pomelo_list_t * list,
    pomelo_list_entry_t * entry
) {
    if (entry == list->front) {
        // Entry is the front of list
        list->front = entry->next;
        if (list->front != NULL) {
            // Non-empty list
            list->front->prev = NULL;
        } else {
            // Emtpy list
            list->back = NULL;
        }
        entry->next = NULL;
    } else if (entry == list->back) {
        list->back = entry->prev;
        if (list->back) {
            // Non-empty list
            list->back->next = NULL;
        } else {
            // Emtpy list
            list->front = NULL;
        }
        entry->prev = NULL;
    } else {
        if (!entry->next && !entry->prev) {
            // The entry has already been removed
            return;
        }

        pomelo_list_entry_t * prev = entry->prev;
        pomelo_list_entry_t * next = entry->next;
        prev->next = next;
        next->prev = prev;
        entry->next = NULL;
        entry->prev = NULL;
    }

    list->size--;
    list->mod_count++;
}


/// @brief Remove an entry
static void pomelo_list_remove_entry(
    pomelo_list_t * list,
    pomelo_list_entry_t * entry
) {
    pomelo_list_unlink_entry(list, entry);
    pomelo_pool_release(list->context->entry_pool, entry);
}

/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */



pomelo_list_context_t * pomelo_list_context_create(
    pomelo_list_context_options_t * options
) {
    assert(options != NULL);

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    pomelo_list_context_t * context =
        pomelo_allocator_malloc_t(allocator, pomelo_list_context_t);
    if (!context) {
        return NULL; // Failed to allocate memory
    }

    memset(context, 0, sizeof(pomelo_list_context_t));
    context->allocator = allocator;
    context->element_size = options->element_size;

    pomelo_pool_root_options_t pool_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_list_entry_t) + options->element_size
    };
    context->entry_pool = pomelo_pool_root_create(&pool_options);
    if (!context->entry_pool) {
        pomelo_list_context_destroy(context);
        return NULL; // Failed to create entry pool
    }

    return context;
}


void pomelo_list_context_destroy(pomelo_list_context_t * context) {
    assert(context != NULL);
    if (context->entry_pool) {
        pomelo_pool_destroy(context->entry_pool);
        context->entry_pool = NULL;
    }

    pomelo_allocator_free(context->allocator, context);
}


pomelo_list_t * pomelo_list_create(pomelo_list_options_t * options) {
    assert(options != NULL);
    if (!options->context && options->element_size == 0) return NULL;

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    // Create list base
    pomelo_list_t * list = pomelo_allocator_malloc_t(allocator, pomelo_list_t);
    if (!list) {
        return NULL;
    }

    memset(list, 0, sizeof(pomelo_list_t));
    list->allocator = allocator;

    if (options->context) {
        list->context_owned = false;
        list->context = options->context;
    } else {
        list->context_owned = true;
        pomelo_list_context_options_t context_options = {
            .allocator = allocator,
            .element_size = options->element_size
        };
        list->context = pomelo_list_context_create(&context_options);
        if (!list->context) {
            pomelo_list_destroy(list);
            return NULL;
        }
    }

#ifndef NDEBUG
    // Set signature for list
    list->signature = POMELO_LIST_SIGNATURE;
    list->entry_signature = entry_signature_generator++;
#endif

    if (options->synchronized) {
        list->mutex = pomelo_mutex_create(allocator);
        if (!list->mutex) {
            pomelo_list_destroy(list);
            return NULL;
        }
    }

    return list;
}


void pomelo_list_destroy(pomelo_list_t * list) {
    assert(list != NULL);
    pomelo_list_check_signature(list);

    // Clear the list to release all entries
    pomelo_list_clear(list);

    // Destroy mutex
    if (list->mutex) {
        pomelo_mutex_destroy(list->mutex);
        list->mutex = NULL;
    }

    if (list->context_owned && list->context) {
        pomelo_list_context_destroy(list->context);
    }
    list->context = NULL;

    // Free the list itself
    pomelo_allocator_free(list->allocator, list);
}


int pomelo_list_resize(pomelo_list_t * list, size_t size) {
    assert(list != NULL);
    bool success = true;

    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    while (list->size < size) {
        // Need more
        pomelo_list_entry_t * entry = pomelo_list_append_back(list);
        if (!entry) {
            success = false;
            break;
        }

        // Zero-initialize the element
        memset(pomelo_list_element_ptr(entry), 0, list->context->element_size);
    }

    while (list->size > size) {
        pomelo_list_remove_entry(list, list->back);
    }

    POMELO_END_CRITICAL_SECTION(mutex);
    return success ? 0 : -1;
}


pomelo_list_entry_t * pomelo_list_push_front_p(
    pomelo_list_t * list,
    void * p_element
) {
    assert(list != NULL);
    assert(p_element != NULL);
    pomelo_list_check_signature(list);
    
    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    pomelo_list_entry_t * entry = pomelo_list_append_front(list);
    if (entry) {
        memcpy(
            pomelo_list_element_ptr(entry),
            p_element,
            list->context->element_size
        );
    }

    POMELO_END_CRITICAL_SECTION(mutex);
    return entry;
}


int pomelo_list_pop_front(pomelo_list_t * list, void * data) {
    assert(list != NULL);
    assert(data != NULL);
    pomelo_list_check_signature(list);
    
    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);
    
    if (!list->front) {
        POMELO_END_CRITICAL_SECTION(mutex);
        return -1;
    }

    if (data) {
        memcpy(
            data,
            pomelo_list_element_ptr(list->front),
            list->context->element_size
        );
    }
    pomelo_list_remove_entry(list, list->front);
    
    POMELO_END_CRITICAL_SECTION(mutex);
    return 0;
}


pomelo_list_entry_t * pomelo_list_push_back_ptr(
    pomelo_list_t * list,
    void * p_element
) {
    assert(list != NULL);
    assert(p_element != NULL);
    pomelo_list_check_signature(list);
    
    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    pomelo_list_entry_t * entry = pomelo_list_append_back(list);
    if (entry) {
        memcpy(
            pomelo_list_element_ptr(entry),
            p_element,
            list->context->element_size
        );
    }

    POMELO_END_CRITICAL_SECTION(mutex);
    return entry;
}


int pomelo_list_pop_back(pomelo_list_t * list, void * data) {
    assert(list != NULL);
    assert(data != NULL);
    pomelo_list_check_signature(list);
    
    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);
    
    if (!list->back) {
        POMELO_END_CRITICAL_SECTION(mutex);
        return -1;
    }

    if (data) {
        memcpy(
            data,
            pomelo_list_element_ptr(list->back),
            list->context->element_size
        );
    }

    pomelo_list_remove_entry(list, list->back);

    POMELO_END_CRITICAL_SECTION(mutex);
    return 0;
}


void pomelo_list_remove(pomelo_list_t * list, pomelo_list_entry_t * entry) {
    assert(list != NULL);
    assert(entry != NULL);
    pomelo_list_check_signature(list);
    pomelo_list_entry_check_signature(list, entry);

    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    pomelo_list_remove_entry(list, entry);

    POMELO_END_CRITICAL_SECTION(mutex);
}


void pomelo_list_clear(pomelo_list_t * list) {
    assert(list != NULL);
    pomelo_list_check_signature(list);

    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    pomelo_list_entry_t * entry = list->front;
    while (entry) {
        pomelo_pool_release(list->context->entry_pool, entry);
        entry = entry->next;
    }

    list->size = 0;
    list->front = NULL;
    list->back = NULL;
    list->mod_count++;

    POMELO_END_CRITICAL_SECTION(mutex);
}


bool pomelo_list_is_empty(pomelo_list_t * list) {
    assert(list != NULL);
    bool result = false;
    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    result = (list->front == NULL);

    POMELO_END_CRITICAL_SECTION(mutex);
    return result;
}


void pomelo_list_iterator_init(
    pomelo_list_iterator_t * it,
    pomelo_list_t * list
) {
    assert(list != NULL);
    assert(it != NULL);

    pomelo_mutex_t * mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    it->list = list;
    it->current = NULL;
    it->mod_count = list->mod_count;

    if (list->size == 0) {
        // This is empty
        it->next = NULL;
    } else {
        it->next = list->front;
    }

    POMELO_END_CRITICAL_SECTION(mutex);
}


int pomelo_list_iterator_next(pomelo_list_iterator_t * it, void * p_element) {
    assert(it != NULL);
    pomelo_mutex_t * mutex = it->list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    assert(it->mod_count == it->list->mod_count);
    if (it->mod_count != it->list->mod_count) {
        assert(false);
        // Mod count has changed
        POMELO_END_CRITICAL_SECTION(mutex);
        return -2;
    }

    if (!it->next) {
        POMELO_END_CRITICAL_SECTION(mutex);
        return -1; // No more elements
    }

    it->current = it->next;
    it->next = it->next->next;

    void * element = pomelo_list_element_ptr(it->current);
    if (p_element) {
        // Clone data
        memcpy(
            p_element,
            element,
            it->list->context->element_size
        );
    }

    POMELO_END_CRITICAL_SECTION(mutex);
    return 0;
}


void pomelo_list_iterator_remove(pomelo_list_iterator_t * it) {
    assert(it != NULL);
    if (!it->current) return;

    pomelo_mutex_t * mutex = it->list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    if (it->mod_count != it->list->mod_count) {
        assert(false);
        // Mod count has changed
        POMELO_END_CRITICAL_SECTION(mutex);
        return;
    }

    pomelo_list_entry_t * entry = it->current;
    it->current = entry->prev;

    // Remove the current entry
    pomelo_list_remove_entry(it->list, entry);

    // Synchronize mod count after removing the entry
    it->mod_count = it->list->mod_count;

    POMELO_END_CRITICAL_SECTION(mutex);
}


pomelo_list_entry_t * pomelo_list_iterator_transfer(
    pomelo_list_iterator_t * it,
    pomelo_list_t * list
) {
    assert(it != NULL);
    assert(list != NULL);

    pomelo_list_t * original_list = it->list;
    assert(original_list->context == list->context);
    if (original_list->context != list->context) {
        return NULL; // Different context, cannot transfer
    }

    pomelo_mutex_t * mutex = original_list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

    pomelo_list_entry_t * entry = it->current;
    it->current = entry->prev;
    pomelo_list_unlink_entry(original_list, entry);

    // Synchronize mod count after unlinking the entry
    it->mod_count = original_list->mod_count;

    POMELO_END_CRITICAL_SECTION(mutex);

    // Add entry to the new list
    mutex = list->mutex;
    POMELO_BEGIN_CRITICAL_SECTION(mutex);

#ifndef NDEBUG
    entry->signature = list->entry_signature;
#endif

    pomelo_list_link_back(list, entry);

    POMELO_END_CRITICAL_SECTION(mutex);
    return entry;
}


/* -------------------------------------------------------------------------- */
/*                               Unrolled list                                */
/* -------------------------------------------------------------------------- */

/// @brief Find the bucket by index
static uint8_t * pomelo_unrolled_list_find_bucket(
    pomelo_unrolled_list_t * list,
    size_t element_index
) {
    assert(list != NULL);

    size_t i;
    size_t bucket_index = element_index / list->bucket_elements;

    pomelo_list_entry_t * entry = NULL;
    if (bucket_index < list->size / 2) {
        // At the left half
        entry = list->entries->front;
        i = 0;

        while (++i < bucket_index) {
            entry = entry->next;
            assert(entry != NULL);
        }
    } else {
        // At the right half
        entry = list->entries->back;
        i = list->size;

        while (--i > bucket_index) {
            entry = entry->prev;
            assert(entry != NULL);
        }
    }

    return pomelo_list_element_ptr(entry);
}


pomelo_unrolled_list_t * pomelo_unrolled_list_create(
    pomelo_unrolled_list_options_t * options
) {
    assert(options != NULL);
    if (options->element_size == 0) return NULL;

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    pomelo_unrolled_list_t * list =
        pomelo_allocator_malloc_t(allocator, pomelo_unrolled_list_t);
    if (!list) return NULL; // Failed to allocate new list

    int ret = pomelo_unrolled_list_init(list, options);
    if (ret < 0) {
        pomelo_unrolled_list_destroy(list);
        return NULL;
    }

    return list;
}


void pomelo_unrolled_list_destroy(pomelo_unrolled_list_t * list) {
    assert(list != NULL);
    pomelo_unrolled_list_finalize(list);
    pomelo_allocator_free(list->allocator, list);
}


int pomelo_unrolled_list_init(
    pomelo_unrolled_list_t * list,
    pomelo_unrolled_list_options_t * options
) {
    assert(list != NULL);
    assert(options != NULL);

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }
    
    size_t bucket_elements = options->bucket_elements;
    if (bucket_elements == 0) {
        bucket_elements = POMELO_UNROLLED_LIST_DEFAULT_ELEMENTS_PER_BUCKET;
    }

    memset(list, 0, sizeof(pomelo_unrolled_list_t));
    list->allocator = allocator;
    list->element_size = options->element_size;
    list->bucket_elements = bucket_elements;

    size_t bucket_size = options->element_size * bucket_elements;

    pomelo_list_options_t list_options = {
        .allocator = allocator,
        .element_size = bucket_size
    };
    list->entries = pomelo_list_create(&list_options);
    if (!list->entries) {
        return -1; // Failed to create entries list
    }

    return 0;
}


void pomelo_unrolled_list_finalize(pomelo_unrolled_list_t * list) {
    assert(list != NULL);

    if (list->entries) {
        pomelo_list_destroy(list->entries);
        list->entries = NULL;
    }
}


int pomelo_unrolled_list_resize(pomelo_unrolled_list_t * list, size_t size) {
    assert(list != NULL);
    
    // The number of bucket
    size_t bucket_size = POMELO_CEIL_DIV(size, list->bucket_elements);
    int ret = pomelo_list_resize(list->entries, bucket_size);
    if (ret < 0) {
        return -1;
    }

    list->size = size;
    return 0;
}


void pomelo_unrolled_list_clear(pomelo_unrolled_list_t * list) {
    assert(list != NULL);

    pomelo_list_clear(list->entries);
    list->size = 0;
}


int pomelo_unrolled_list_get(
    pomelo_unrolled_list_t * list,
    size_t index,
    void * p_element
) {
    assert(list != NULL);
    assert(p_element != NULL);

    if (index >= list->size) {
        // Out of bound
        return -1;
    }

    uint8_t * bucket = pomelo_unrolled_list_find_bucket(list, index);
    size_t offset = (index % list->bucket_elements) * list->element_size;
    memcpy(p_element, bucket + offset, list->element_size);

    return 0;
}


int pomelo_unrolled_list_get_back(
    pomelo_unrolled_list_t * list,
    void * p_element
) {
    assert(list != NULL);
    assert(p_element != NULL);

    if (list->size == 0) {
        return -1;
    }

    uint8_t * bucket = pomelo_list_element_ptr(list->entries->back);
    size_t offset_index = ((list->size - 1) % list->bucket_elements);
    size_t offset = offset_index * list->element_size;
    memcpy(p_element, bucket + offset, list->element_size);

    return 0;
}


int pomelo_unrolled_list_get_front(
    pomelo_unrolled_list_t * list,
    void * p_element
) {
    assert(list != NULL);
    assert(p_element != NULL);

    if (list->size == 0) {
        return -1;
    }

    uint8_t * bucket = pomelo_list_element_ptr(list->entries->front);
    memcpy(p_element, bucket, list->element_size);

    return 0;
}


void * pomelo_unrolled_list_set_ptr(
    pomelo_unrolled_list_t * list,
    size_t index,
    void * p_element
) {
    assert(list != NULL);
    assert(p_element != NULL);

    if (index >= list->size) return NULL;

    uint8_t * bucket = pomelo_unrolled_list_find_bucket(list, index);
    size_t offset = (index % list->bucket_elements) * list->element_size;
    void * element = bucket + offset;
    memcpy(element, p_element, list->element_size);

    return element;
}


void * pomelo_unrolled_list_push_back_ptr(
    pomelo_unrolled_list_t * list,
    void * p_element
) {
    assert(list != NULL);
    assert(p_element != NULL);

    size_t new_size = list->size + 1;
    size_t new_bucket_size = POMELO_CEIL_DIV(new_size, list->bucket_elements);
    
    if (new_bucket_size != list->entries->size) {
        int ret = pomelo_list_resize(list->entries, new_bucket_size);
        if (ret < 0) return NULL;
    }

    uint8_t * bucket = pomelo_list_element_ptr(list->entries->back);
    size_t offset = (list->size % list->bucket_elements) * list->element_size;
    void * element = bucket + offset;
    memcpy(element, p_element, list->element_size);

    list->size++;
    return element;
}


int pomelo_unrolled_list_pop_back(
    pomelo_unrolled_list_t * list,
    void * p_element
) {
    assert(list != NULL);

    if (list->size == 0) {
        // List is empty
        return -1;
    }

    if (p_element) {
        uint8_t * bucket = pomelo_list_element_ptr(list->entries->back);
        size_t offset_index = ((list->size - 1) % list->bucket_elements);
        size_t offset = offset_index * list->element_size;
        memcpy(p_element, bucket + offset, list->element_size);
    }

    size_t new_size = list->size - 1;
    size_t new_bucket_size = POMELO_CEIL_DIV(new_size, list->bucket_elements);
    if (new_bucket_size != list->entries->size) {
        int ret = pomelo_list_resize(list->entries, new_bucket_size);
        if (ret < 0) {
            return -1;
        }
    }

    list->size--;
    return 0;
}


void pomelo_unrolled_list_begin(
    pomelo_unrolled_list_t * list,
    pomelo_unrolled_list_iterator_t * it
) {
    assert(list != NULL);
    assert(it != NULL);

    it->list = list;

    if (list->size == 0) {
        it->entry = NULL;
        it->bucket = NULL;
        it->index = 0;
        return;
    }

    it->entry = list->entries->front;
    it->bucket = pomelo_list_element_ptr(it->entry);
    it->index = 0;
}


void pomelo_unrolled_list_end(
    pomelo_unrolled_list_t * list,
    pomelo_unrolled_list_iterator_t * it
) {
    assert(list != NULL);
    assert(it != NULL);

    it->list = list;

    if (list->size == 0) {
        it->entry = NULL;
        it->bucket = NULL;
        it->index = 0;
        return;
    }

    it->entry = list->entries->back;
    it->bucket = pomelo_list_element_ptr(it->entry);
    it->index = list->size - 1;
}


int pomelo_unrolled_list_iterator_next(
    pomelo_unrolled_list_iterator_t * it,
    void * output
) {
    assert(it != NULL);
    if (it->index >= it->list->size) {
        return -1; // End of iteration
    }


    pomelo_unrolled_list_t * list = it->list;
    size_t bucket_index = it->index % list->bucket_elements;

    void * element = it->bucket + bucket_index * list->element_size;
    if (output) {
        memcpy(output, element, list->element_size);
    }

    if (bucket_index == list->bucket_elements - 1) {
        // Next bucket
        it->entry = it->entry->next;
        it->bucket = it->entry ? pomelo_list_element_ptr(it->entry) : NULL;
    }

    ++it->index;
    return 0;
}


int pomelo_unrolled_list_iterator_prev(
    pomelo_unrolled_list_iterator_t * it,
    void * output
) {
    assert(it != NULL);
    if (it->index >= it->list->size) {
        return -1; // End of iteration
    }

    pomelo_unrolled_list_t * list = it->list;
    size_t bucket_index = it->index % list->bucket_elements;
    void * element = it->bucket + bucket_index * list->element_size;
    if (output) {
        memcpy(output, element, list->element_size);
    }

    if (bucket_index == 0) {
        // Previous bucket
        it->entry = it->entry->prev;
        it->bucket = it->entry ? pomelo_list_element_ptr(it->entry) : NULL;
    }

    --it->index;
    return 0;
}
