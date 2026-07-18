#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "compact_diag.h"

int main(void)
{
    compact_diag_tracker tracker;
    uint64_t elapsed_ms = 0;

    compact_diag_tracker_init(&tracker, 1000);

    assert(compact_diag_observe(&tracker, 1499, 0, &elapsed_ms) ==
           COMPACT_DIAG_NONE);
    assert(compact_diag_observe(&tracker, 1500, 0, &elapsed_ms) ==
           COMPACT_DIAG_STALL_START);
    assert(elapsed_ms == 500);
    assert(compact_diag_observe(&tracker, 2499, 0, &elapsed_ms) ==
           COMPACT_DIAG_NONE);
    assert(compact_diag_observe(&tracker, 2500, 0, &elapsed_ms) ==
           COMPACT_DIAG_STALL_ONGOING);
    assert(elapsed_ms == 1500);
    assert(compact_diag_observe(&tracker, 2700, 1, &elapsed_ms) ==
           COMPACT_DIAG_STALL_RECOVERED);
    assert(elapsed_ms == 1700);

    assert(compact_diag_observe(&tracker, 3000, 1, &elapsed_ms) ==
           COMPACT_DIAG_NONE);
    assert(compact_diag_observe(&tracker, 3499, 0, &elapsed_ms) ==
           COMPACT_DIAG_NONE);
    assert(compact_diag_observe(&tracker, 3500, 0, &elapsed_ms) ==
           COMPACT_DIAG_STALL_START);
    assert(elapsed_ms == 500);

    puts("compact diag tracker tests passed");
    return 0;
}
