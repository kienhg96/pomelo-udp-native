#ifndef POMELO_TEST_BASE_TEST_H
#define POMELO_TEST_BASE_TEST_H
#ifdef __cplusplus
extern "C" {
#endif


int pomelo_test_address(void);
int pomelo_test_payload(void);
int pomelo_test_allocator(void);
int pomelo_test_reference(void);
int pomelo_test_reference_finalize_once(void);


#ifdef __cplusplus
}
#endif
#endif // POMELO_TEST_BASE_TEST_H
