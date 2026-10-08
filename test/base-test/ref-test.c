#include <stdbool.h>
#include "pomelo-test.h"
#include "base/ref.h"
#include "base-test.h"


static bool finalized = false;


static void reference_finalize(pomelo_reference_t * reference) {
    (void) reference;
    finalized = true;
}


int pomelo_test_reference(void) {
    pomelo_reference_t ref;
    pomelo_reference_init(&ref, reference_finalize);

    pomelo_check(pomelo_reference_ref_count(&ref) == 1);
    pomelo_check(pomelo_reference_ref(&ref) == true);
    pomelo_check(pomelo_reference_ref_count(&ref) == 2);
    pomelo_check(finalized == false);
    pomelo_reference_unref(&ref);
    pomelo_check(pomelo_reference_ref_count(&ref) == 1);
    pomelo_check(finalized == false);
    pomelo_reference_unref(&ref);
    pomelo_check(finalized == true);
    pomelo_check(pomelo_reference_ref_count(&ref) == 0);

    return 0;
}


/* -------------------------------------------------------------------------- */
/*            Finalize-once lifecycle (callback invocation count)             */
/* -------------------------------------------------------------------------- */

static int finalize_count = 0;


static void reference_count_finalize(pomelo_reference_t * reference) {
    (void) reference;
    finalize_count++;
}


/// @brief Drive a full ref/unref lifecycle with multiple outstanding refs and
/// assert the finalize callback fires exactly once, only when the count hits 0.
int pomelo_test_reference_finalize_once(void) {
    finalize_count = 0;

    pomelo_reference_t ref;
    pomelo_reference_init(&ref, reference_count_finalize);
    pomelo_check(pomelo_reference_ref_count(&ref) == 1);

    // Take several extra references.
    pomelo_check(pomelo_reference_ref(&ref) == true); // count 2
    pomelo_check(pomelo_reference_ref(&ref) == true); // count 3
    pomelo_check(pomelo_reference_ref(&ref) == true); // count 4
    pomelo_check(pomelo_reference_ref_count(&ref) == 4);

    // Drop them one by one; the callback must not fire until count reaches 0.
    pomelo_reference_unref(&ref); // 3
    pomelo_check(finalize_count == 0);
    pomelo_reference_unref(&ref); // 2
    pomelo_check(finalize_count == 0);
    pomelo_reference_unref(&ref); // 1
    pomelo_check(finalize_count == 0);
    pomelo_check(pomelo_reference_ref_count(&ref) == 1);

    // The final unref drives the count to 0 and fires the callback exactly once.
    pomelo_reference_unref(&ref); // 0
    pomelo_check(pomelo_reference_ref_count(&ref) == 0);
    pomelo_check(finalize_count == 1);

    /*
     * NOTE: the "refuse to revive a finalized reference" CAS branch in
     * pomelo_reference_ref() (ref.c: `if (ref <= 0) { assert(false ...);
     * return false; }`) cannot be exercised in the default (debug) build:
     * calling pomelo_reference_ref() on a finalized reference trips the
     * `assert(false)` and aborts the process BEFORE the `return false` path
     * is observable. Driving it requires an NDEBUG build, where the assert is
     * compiled out and the function returns false instead of aborting.
     *
     * Deliberately NOT attempting it here to avoid aborting the suite and to
     * avoid weakening src/. Under NDEBUG the check would be:
     *
     *     // (NDEBUG build only)
     *     // pomelo_check(pomelo_reference_ref(&ref) == false);
     *     // pomelo_check(pomelo_reference_ref_count(&ref) == 0);
     *     // pomelo_check(finalize_count == 1); // still only finalized once
     */

    return 0;
}
