#include "pomelo-test.h"
#include "base-test.h"


int main(void) {
    printf("Base test\n");

    pomelo_run_test(pomelo_test_address);
    pomelo_run_test(pomelo_test_payload);
    pomelo_run_test(pomelo_test_allocator);
    pomelo_run_test(pomelo_test_reference);
    pomelo_run_test(pomelo_test_reference_finalize_once);

    printf("*** All base tests passed ***\n");
    return 0;
}
