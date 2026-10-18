// OHOS 兼容替身：mach 时钟 → clock_gettime(CLOCK_MONOTONIC)，时基 1:1 纳秒
// OHOS compat shim: mach clock -> clock_gettime(CLOCK_MONOTONIC), timebase 1:1 ns
#pragma once
#include <stdint.h>
#include <time.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline uint64_t mach_absolute_time(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline int mach_timebase_info(mach_timebase_info_data_t *tb) { tb->numer = 1; tb->denom = 1; return 0; }
