// OHOS 兼容替身：指令缓存刷新 → 编译器内建
// OHOS compat shim: instruction cache flush -> compiler builtin
#pragma once
#include <stddef.h>
static inline void sys_icache_invalidate(void *start, size_t len) { __builtin___clear_cache((char *)start, (char *)start + len); }
static inline void sys_dcache_flush(void *start, size_t len) { (void)start; (void)len; }
