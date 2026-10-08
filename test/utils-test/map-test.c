#include <string.h>
#include "pomelo-test.h"
#include "utils/map.h"
#include "utils-test.h"


int pomelo_test_map(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    pomelo_map_options_t options = {
        .allocator = allocator,
        .key_size = sizeof(int),
        .value_size = sizeof(int),
    };
    pomelo_map_t * map = pomelo_map_create(&options);
    pomelo_check(map != NULL);

    int key = 1;
    int value = 1000;

    pomelo_check(pomelo_map_set(map, key, value) != NULL);

    int val_read = 0;
    pomelo_check(pomelo_map_get(map, key, &val_read) == 0);
    pomelo_check(val_read == value);
    pomelo_check(map->size == 1);

    // Update the existent key
    value = 2000;
    pomelo_check(pomelo_map_set(map, key, value) != NULL);
    pomelo_check(map->size == 1); // Unchanged
    pomelo_check(pomelo_map_get(map, key, &val_read) == 0);
    pomelo_check(val_read == value);

    key = 2;
    // Get non-existent key
    pomelo_check(pomelo_map_get(map, key, &val_read) < 0);

    value = 3000;
    pomelo_check(pomelo_map_set(map, key, value) != NULL);
    pomelo_check(pomelo_map_has(map, key));
    pomelo_check(map->size == 2);

    // Delete existent key
    pomelo_check(pomelo_map_del(map, key) == 0);
    pomelo_check(map->size == 1);

    pomelo_check(pomelo_map_del(map, key) < 0);
    pomelo_check(!pomelo_map_has(map, key));

    pomelo_check(map->size == 1);

    // Destroy the map
    pomelo_map_destroy(map);

    // Check memleak
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}


/// @brief Insert enough entries to force one or more rehash/grow operations and
/// verify every key still resolves to its value after redistribution.
int pomelo_test_map_rehash(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    // Start with the default 16 buckets / 0.75 load factor. Inserting 200
    // distinct keys forces the table to grow several times (16 -> 32 -> 64 ...).
    pomelo_map_options_t options = {
        .allocator = allocator,
        .key_size = sizeof(int),
        .value_size = sizeof(int),
    };
    pomelo_map_t * map = pomelo_map_create(&options);
    pomelo_check(map != NULL);

    const int N = 200;
    size_t initial_buckets = map->buckets->size;

    for (int i = 0; i < N; ++i) {
        int k = i;
        int v = i * 10 + 7;
        pomelo_check(pomelo_map_set(map, k, v) != NULL);
    }
    pomelo_check(map->size == (size_t) N);

    // The table must have grown (at least one rehash happened).
    pomelo_check(map->buckets->size > initial_buckets);

    // Every key must still map to its value after the rehash redistribution.
    for (int i = 0; i < N; ++i) {
        int k = i;
        int out = -1;
        pomelo_check(pomelo_map_get(map, k, &out) == 0);
        pomelo_check(out == i * 10 + 7);
        pomelo_check(pomelo_map_has(map, k));
    }

    // Delete every key; map should be empty afterwards.
    for (int i = 0; i < N; ++i) {
        int k = i;
        pomelo_check(pomelo_map_del(map, k) == 0);
    }
    pomelo_check(map->size == 0);

    pomelo_map_destroy(map);
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}


/// @brief Insert keys that collide into the same bucket (spaced by the bucket
/// count) with a load factor high enough to suppress any rehash, so the
/// separate-chaining get/del path is exercised directly.
int pomelo_test_map_collision(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    // 8 buckets, huge load factor => no rehash. The default int hash is the
    // key value itself, so keys spaced by 8 all land in bucket 0.
    const size_t NBUCKETS = 8;
    pomelo_map_options_t options = {
        .allocator = allocator,
        .key_size = sizeof(int),
        .value_size = sizeof(int),
        .initial_buckets = NBUCKETS,
        .load_factor = 1000.0f,
    };
    pomelo_map_t * map = pomelo_map_create(&options);
    pomelo_check(map != NULL);

    const int COUNT = 6;
    int keys[COUNT];
    for (int i = 0; i < COUNT; ++i) {
        keys[i] = (int) (i * NBUCKETS); // 0, 8, 16, 24, 32, 40
        int v = keys[i] * 3 + 1;
        pomelo_check(pomelo_map_set(map, keys[i], v) != NULL);
    }
    pomelo_check(map->size == (size_t) COUNT);

    // No rehash must have happened: still the same number of buckets, so these
    // all share a single bucket chain.
    pomelo_check(map->buckets->size == NBUCKETS);

    // Each colliding key resolves to its own value along the chain.
    for (int i = 0; i < COUNT; ++i) {
        int out = -1;
        pomelo_check(pomelo_map_get(map, keys[i], &out) == 0);
        pomelo_check(out == keys[i] * 3 + 1);
    }

    // Delete a middle element of the chain; the rest must still resolve.
    int victim = keys[2];
    pomelo_check(pomelo_map_del(map, victim) == 0);
    pomelo_check(!pomelo_map_has(map, victim));
    pomelo_check(map->size == (size_t) (COUNT - 1));

    for (int i = 0; i < COUNT; ++i) {
        if (keys[i] == victim) continue;
        int out = -1;
        pomelo_check(pomelo_map_get(map, keys[i], &out) == 0);
        pomelo_check(out == keys[i] * 3 + 1);
    }

    // Deleting the already-removed key fails.
    pomelo_check(pomelo_map_del(map, victim) < 0);

    // Clean up the rest of the chain.
    for (int i = 0; i < COUNT; ++i) {
        if (keys[i] == victim) continue;
        pomelo_check(pomelo_map_del(map, keys[i]) == 0);
    }
    pomelo_check(map->size == 0);

    pomelo_map_destroy(map);
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}


/// @brief Verify the iterator visits every inserted entry exactly once, across
/// several buckets, with the correct key/value pairing.
int pomelo_test_map_iterator(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    pomelo_map_options_t options = {
        .allocator = allocator,
        .key_size = sizeof(int),
        .value_size = sizeof(int),
    };
    pomelo_map_t * map = pomelo_map_create(&options);
    pomelo_check(map != NULL);

    const int N = 50;
    int visited[50];
    memset(visited, 0, sizeof(visited));

    for (int i = 0; i < N; ++i) {
        int k = i;
        int v = i * 10 + 7;
        pomelo_check(pomelo_map_set(map, k, v) != NULL);
    }
    pomelo_check(map->size == (size_t) N);

    pomelo_map_iterator_t it;
    pomelo_map_iterator_init(&it, map);

    pomelo_map_entry_t * entry = NULL;
    int seen = 0;
    while (pomelo_map_iterator_next(&it, &entry) == 0) {
        int ekey = *((int *) entry->p_key);
        int eval = pomelo_map_entry_value(entry, int);
        pomelo_check(ekey >= 0 && ekey < N);
        pomelo_check(eval == ekey * 10 + 7); // Correct key/value pairing
        pomelo_check(visited[ekey] == 0);    // Each entry visited at most once
        visited[ekey] = 1;
        seen++;
    }

    // Every inserted entry visited exactly once.
    pomelo_check(seen == N);
    for (int i = 0; i < N; ++i) {
        pomelo_check(visited[i] == 1);
    }

    pomelo_map_destroy(map);
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}
