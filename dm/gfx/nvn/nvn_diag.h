// Small, platform-independent diagnostic safety helpers. No Vulkan dependency.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline size_t nvn_log_length(int formatted, size_t capacity)
{
    return formatted <= 0 || !capacity ? 0 :
           (size_t)formatted < capacity ? (size_t)formatted : capacity - 1;
}

// Subtract only after checking off: addition-based validation can wrap uint32_t.
static inline int nvn_range_valid(uint32_t off, uint32_t count, uint32_t total)
{
    return off <= total && count <= total - off;
}

enum { NVN_TRACE_BIND, NVN_TRACE_QUEUE, NVN_TRACE_ATTEMPT,
       NVN_TRACE_RESULT, NVN_TRACE_DROP, NVN_TRACE_KINDS };
#define NVN_TRACE_BUDGET 12
#define NVN_TRACE_WINDOW 60

typedef struct {
    uint64_t window, emitted[NVN_TRACE_KINDS], suppressed[NVN_TRACE_KINDS];
    uint64_t seen[NVN_TRACE_KINDS][NVN_TRACE_BUDGET];
    int started;
} nvn_trace_budget;

// Caller reports prior-window counts BEFORE rotating. Budgets renew throughout
// world entry, rather than being consumed permanently by the opening menu.
static inline int nvn_trace_rotate(nvn_trace_budget *b, uint64_t frame)
{
    uint64_t window = frame / NVN_TRACE_WINDOW;
    if (b->started && window == b->window) return 0;
    memset(b, 0, sizeof *b);
    b->window = window;
    b->started = 1;
    return 1;
}

// Distinct signatures per event/window; duplicate events do not consume budget.
// Suppressed includes duplicates and capacity drops, so absence is never proof.
static inline int nvn_trace_allow(nvn_trace_budget *b, unsigned event, uint64_t signature)
{
    if (event >= NVN_TRACE_KINDS) return 0;
    uint64_t n = b->emitted[event];
    for (uint64_t i = 0; i < n; i++)
        if (b->seen[event][i] == signature) { b->suppressed[event]++; return 0; }
    if (n == NVN_TRACE_BUDGET) { b->suppressed[event]++; return 0; }
    b->seen[event][n] = signature;
    b->emitted[event]++;
    return 1;
}
