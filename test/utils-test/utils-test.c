#include "pomelo-test.h"
#include "utils-test.h"


int main(void) {
    printf("Utils test\n");

    pomelo_run_test(pomelo_test_pool);
    pomelo_run_test(pomelo_test_list);
    pomelo_run_test(pomelo_test_list_transfer_integrity);
    pomelo_run_test(pomelo_test_unrolled_list);
    pomelo_run_test(pomelo_test_array);
    pomelo_run_test(pomelo_test_map);
    pomelo_run_test(pomelo_test_map_rehash);
    pomelo_run_test(pomelo_test_map_collision);
    pomelo_run_test(pomelo_test_map_iterator);
    pomelo_run_test(pomelo_test_heap);

    printf("*** All utils tests passed ***\n");
    return 0;
}
