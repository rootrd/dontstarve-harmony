#include "../nvn/nvn_diag.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>

int main(void)
{
    assert(nvn_log_length(-1, 10) == 0);
    assert(nvn_log_length(0, 10) == 0);
    assert(nvn_log_length(5, 10) == 5);
    assert(nvn_log_length(10, 10) == 9);
    assert(nvn_log_length(INT_MAX, 1200) == 1199);
    assert(nvn_log_length(3, 0) == 0);
    assert(nvn_log_length(3, 1) == 0);
    // Exercise the exact bounded-write idiom under ASan with truncating snprintf.
    char buf[16];
    volatile int width = 1000;
    int n = snprintf(buf, sizeof buf, "%*s", width, "x");
    FILE *f = tmpfile(); assert(f);
    assert(fwrite(buf, 1, nvn_log_length(n, sizeof buf), f) == 15);
    assert(ftell(f) == 15); fclose(f);

    assert(nvn_range_valid(0, 0, 0));
    assert(nvn_range_valid(3, 2, 5));
    assert(nvn_range_valid(5, 0, 5));
    assert(!nvn_range_valid(5, 1, 5));
    assert(!nvn_range_valid(6, 0, 5));
    assert(!nvn_range_valid(UINT32_MAX, 1, 4));
    assert(!nvn_range_valid(UINT32_MAX - 1, 4, 4));
    assert(nvn_range_valid(UINT32_MAX - 1, 1, UINT32_MAX));

    nvn_trace_budget b = {0};
    assert(nvn_trace_rotate(&b, 0));
    for (unsigned event = 0; event < NVN_TRACE_KINDS; event++) {
        for (unsigned i = 0; i < NVN_TRACE_BUDGET; i++)
            assert(nvn_trace_allow(&b, event, i));
        assert(!nvn_trace_allow(&b, event, 999));
        assert(!nvn_trace_allow(&b, event, 0));
        assert(b.emitted[event] == NVN_TRACE_BUDGET);
        assert(b.suppressed[event] == 2);
    }
    assert(!nvn_trace_allow(&b, NVN_TRACE_KINDS, 0));
    assert(!nvn_trace_rotate(&b, 59));
    assert(nvn_trace_rotate(&b, 60));
    assert(b.suppressed[0] == 0 && b.emitted[0] == 0);
    assert(nvn_trace_allow(&b, NVN_TRACE_BIND, 0));
    assert(!nvn_trace_allow(&b, NVN_TRACE_BIND, 0));
    assert(nvn_trace_allow(&b, NVN_TRACE_QUEUE, 0));
    // Opening-menu saturation does not consume the in-world window.
    assert(nvn_trace_rotate(&b, 10000));
    assert(nvn_trace_allow(&b, NVN_TRACE_BIND, 0));
    puts("PASS: log bounds, overflow-safe ranges, per-event window budgets");
    return 0;
}
