#include <string.h>
#include "pomelo-test.h"
#include "utils/list.h"
#include "utils-test.h"


int pomelo_test_list(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);
    pomelo_list_options_t options = {
        .element_size = sizeof(int),
        .allocator = allocator,
        .synchronized = true,
    };

    // Create the list
    pomelo_list_t * list = pomelo_list_create(&options);
    pomelo_check(list != NULL);
    pomelo_check(alloc_bytes < pomelo_allocator_allocated_bytes(allocator));

    pomelo_check(list->size == 0);

    int value = 1;
    pomelo_check(pomelo_list_push_back(list, value) != NULL);
    pomelo_check(list->size == 1);

    value = 2;
    pomelo_list_entry_t * second = pomelo_list_push_back(list, value);
    pomelo_check(second != NULL);
    pomelo_check(list->size == 2);

    value = 3;
    pomelo_check(pomelo_list_push_back(list, value) != NULL);
    pomelo_check(list->size == 3);

    pomelo_list_remove(list, second);
    pomelo_check(list->size == 2);

    value = 4;
    pomelo_check(pomelo_list_push_back(list, value) != NULL);
    pomelo_check(list->size == 3);

    value = 5;
    pomelo_check(pomelo_list_push_front(list, value) != NULL);
    pomelo_check(list->size == 4);

    printf("List values: ");
    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, list);
    while (pomelo_list_iterator_next(&it, &value) == 0) {
        printf("%d, ", value);
    }
    printf("\n");

    // The list data: [5, 1, 3, 4]
    pomelo_check(pomelo_list_pop_front(list, &value) == 0);
    pomelo_check(value == 5);
    pomelo_check(pomelo_list_pop_front(list, &value) == 0);
    pomelo_check(value == 1);
    pomelo_check(pomelo_list_pop_back(list, &value) == 0);
    pomelo_check(value == 4);
    pomelo_check(pomelo_list_pop_back(list, &value) == 0);
    pomelo_check(value == 3);
    pomelo_check(pomelo_list_pop_back(list, &value) != 0);
    pomelo_check(pomelo_list_pop_front(list, &value) != 0);

    pomelo_check(list->front == NULL);
    pomelo_check(list->back == NULL);
    pomelo_check(list->size == 0);

    pomelo_list_destroy(list);

    // Create a list context with options
    pomelo_list_context_options_t ctx_options = {
        .element_size = sizeof(int),
        .allocator = allocator,
    };
    pomelo_list_context_t * context = pomelo_list_context_create(&ctx_options);
    pomelo_check(context != NULL);

    // Create two lists sharing the same context
    memset(&options, 0, sizeof(pomelo_list_options_t));
    options.allocator = allocator;
    options.context = context;
    pomelo_list_t * list1 = pomelo_list_create(&options);
    pomelo_check(list1 != NULL);

    memset(&options, 0, sizeof(pomelo_list_options_t));
    options.allocator = allocator;
    options.context = context;
    pomelo_list_t * list2 = pomelo_list_create(&options);
    pomelo_check(list2 != NULL);

    // Add 3 elements to first list
    value = 10;
    pomelo_check(pomelo_list_push_back(list1, value) != NULL);
    value = 20;
    pomelo_check(pomelo_list_push_back(list1, value) != NULL);
    value = 30;
    pomelo_check(pomelo_list_push_back(list1, value) != NULL);
    pomelo_check(list1->size == 3);

    // Create iterator for first list
    pomelo_list_iterator_init(&it, list1);

    // Iterate the first element
    pomelo_list_iterator_next(&it, &value);
    pomelo_check(value == 10);
    
    // Transfer front element to second list
    pomelo_check(pomelo_list_iterator_transfer(&it, list2) != NULL);
    pomelo_check(list1->size == 2);
    pomelo_check(list2->size == 1);

    // Verify the transfer
    pomelo_check(pomelo_list_pop_front(list2, &value) == 0);
    pomelo_check(value == 10);

    // Clean up
    pomelo_list_destroy(list1);
    pomelo_list_destroy(list2);
    pomelo_list_context_destroy(context);

    // Check memleak
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}


int pomelo_test_unrolled_list(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    pomelo_unrolled_list_options_t options = {
        .allocator = allocator,
        .element_size = sizeof(int),
        .bucket_elements = 16,
    };
    int ret, value;

    // Create the list
    pomelo_unrolled_list_t * list = pomelo_unrolled_list_create(&options);
    pomelo_check(list != NULL);
    
    pomelo_check(list->size == 0);

    ret = pomelo_unrolled_list_pop_back(list, &value);
    pomelo_check(ret != 0);

    // Test push back API
    for (size_t i = 0; i < 33; i++) {
        pomelo_check(pomelo_unrolled_list_push_back(list, i) != NULL);
        pomelo_check(list->size == (i + 1));
    }

    pomelo_check(list->entries->size == 3);

    // Test set/get API
    ret = pomelo_unrolled_list_get(list, 9, &value);
    pomelo_check(ret == 0);
    pomelo_check(value == 9);

    value = 90;
    void * element = pomelo_unrolled_list_set(list, 10, value);
    pomelo_check(element != NULL);

    element = pomelo_unrolled_list_set(list, 33, value);
    pomelo_check(element == NULL);

    // Test pop back API
    ret = pomelo_unrolled_list_pop_back(list, &value);
    pomelo_check(ret == 0);

    pomelo_check(value == 32);
    pomelo_check(list->size == 32);

    pomelo_check(list->entries->size == 2);

    // Test iterator
    pomelo_unrolled_list_iterator_t it;
    pomelo_unrolled_list_begin(list, &it);

    size_t index = 0;
    while (pomelo_unrolled_list_iterator_next(&it, &value)) {
        if (index == 10) {
            pomelo_check(value == 90);
        } else {
            pomelo_check(value == (int) index);
        }
        index++;
    }

    pomelo_unrolled_list_end(list, &it);
    index = list->size - 1;
    while (pomelo_unrolled_list_iterator_prev(&it, &value)) {
        if (index == 10) {
            pomelo_check(value == 90);
        } else {
            pomelo_check(value == (int) index);
        }
        index--;
    }

    pomelo_unrolled_list_destroy(list);

    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}


// Regression: pomelo_list_iterator_transfer must (a) keep the source iterator's
// mod_count in sync after the unlink so iteration can continue, and (b) stamp
// the moved entry with the destination list's signature so it can later be
// removed from the destination with pomelo_list_remove. Both are Debug-build
// integrity guards; before the fix each path aborted. Surfaced by the delivery
// soak (map bucket redistribute uses iterator_transfer once a map grows).
int pomelo_test_list_transfer_integrity(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    pomelo_list_context_options_t ctx_options = {
        .element_size = sizeof(int),
        .allocator = allocator,
    };
    pomelo_list_context_t * context = pomelo_list_context_create(&ctx_options);
    pomelo_check(context != NULL);

    pomelo_list_options_t options;
    memset(&options, 0, sizeof(options));
    options.allocator = allocator;
    options.context = context;
    pomelo_list_t * list1 = pomelo_list_create(&options);
    pomelo_check(list1 != NULL);

    memset(&options, 0, sizeof(options));
    options.allocator = allocator;
    options.context = context;
    pomelo_list_t * list2 = pomelo_list_create(&options);
    pomelo_check(list2 != NULL);

    int value;
    value = 10; pomelo_check(pomelo_list_push_back(list1, value) != NULL);
    value = 20; pomelo_check(pomelo_list_push_back(list1, value) != NULL);
    value = 30; pomelo_check(pomelo_list_push_back(list1, value) != NULL);
    pomelo_check(list1->size == 3);

    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, list1);

    // Advance to the middle element (20), then transfer it to list2.
    pomelo_check(pomelo_list_iterator_next(&it, &value) == 0);
    pomelo_check(value == 10);
    pomelo_check(pomelo_list_iterator_next(&it, &value) == 0);
    pomelo_check(value == 20);

    pomelo_list_entry_t * moved = pomelo_list_iterator_transfer(&it, list2);
    pomelo_check(moved != NULL);
    pomelo_check(list1->size == 2);
    pomelo_check(list2->size == 1);

    // (a) Continue iterating list1 after the transfer. Without the mod_count
    // sync this aborts on the iterator's concurrent-modification assert.
    int seen = 0;
    while (pomelo_list_iterator_next(&it, &value) == 0) {
        pomelo_check(value == 30);
        seen++;
    }
    pomelo_check(seen == 1);

    // (b) Remove the moved entry from list2 via pomelo_list_remove. Without the
    // signature stamp this aborts on the entry-signature assert.
    pomelo_list_remove(list2, moved);
    pomelo_check(list2->size == 0);

    pomelo_list_destroy(list1);
    pomelo_list_destroy(list2);
    pomelo_list_context_destroy(context);

    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}
