#ifndef POMELO_TEST_UTILS_TEST_H
#define POMELO_TEST_UTILS_TEST_H
#ifdef __cplusplus
extern "C" {
#endif


int pomelo_test_pool(void);
int pomelo_test_list(void);
int pomelo_test_unrolled_list(void);
int pomelo_test_array(void);
int pomelo_test_map(void);
int pomelo_test_map_rehash(void);
int pomelo_test_map_collision(void);
int pomelo_test_map_iterator(void);
int pomelo_test_heap(void);


#ifdef __cplusplus
}
#endif // __cplusplus
#endif // POMELO_TEST_UTILS_TEST_H
