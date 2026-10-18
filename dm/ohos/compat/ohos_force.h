// 强制前置包含（Makefile 用 -include 注入全部 TU）：OHOS 上缺失的小接口与 Apple 惯用式的跨平台映射
// Force-included via -include for every TU: small interfaces missing on OHOS and cross-platform mappings for Apple idioms.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/xattr.h>
#include <TargetConditionals.h>   // 全局可得（多个 TU 用 TARGET_OS_* 但未自行包含）

#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t cid) {
    struct timespec ts; clock_gettime(cid, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#ifndef st_mtimespec
#define st_mtimespec st_mtim
#endif

// Apple 的 pthread 栈查询 → musl pthread_getattr_np
static inline void *pthread_get_stackaddr_np(pthread_t t) {
    pthread_attr_t a; void *base = 0; size_t size = 0;
    if (pthread_getattr_np(t, &a)) return 0;
    pthread_attr_getstack(&a, &base, &size); pthread_attr_destroy(&a);
    return (char *)base + size;
}
static inline size_t pthread_get_stacksize_np(pthread_t t) {
    pthread_attr_t a; void *base = 0; size_t size = 0;
    if (pthread_getattr_np(t, &a)) return 0;
    pthread_attr_getstack(&a, &base, &size); pthread_attr_destroy(&a);
    return size;
}

// ---- ucontext：Apple 的 uc->uc_mcontext->__ss/__es 访问式 → 跨平台宏（源文件已改用 OC_*）----
#if defined(__APPLE__)
#define OC_SS_PC(uc)  ((uc)->uc_mcontext->__ss.__pc)
#define OC_SS_LR(uc)  ((uc)->uc_mcontext->__ss.__lr)
#define OC_SS_SP(uc)  ((uc)->uc_mcontext->__ss.__sp)
#define OC_SS_FP(uc)  ((uc)->uc_mcontext->__ss.__fp)
#define OC_SS_X(uc,i) ((uc)->uc_mcontext->__ss.__x[i])
#define OC_ES_ESR(uc) ((uc)->uc_mcontext->__es.__esr)
#else
#define OC_SS_PC(uc)  ((uc)->uc_mcontext.pc)
#define OC_SS_LR(uc)  ((uc)->uc_mcontext.regs[30])
#define OC_SS_SP(uc)  ((uc)->uc_mcontext.sp)
#define OC_SS_FP(uc)  ((uc)->uc_mcontext.regs[29])
#define OC_SS_X(uc,i) ((uc)->uc_mcontext.regs[i])
#define OC_ES_ESR(uc) (0u)   // Linux 不暴露 ESR；fault 判定退回 si_code / si_addr
#endif

// ---- xattr：Apple 6 参风格 → OHOS 4 参 + "user." 前缀；ENOATTR → ENODATA ----
#ifndef XATTR_NOFOLLOW
#define XATTR_NOFOLLOW 0x0001
#endif
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
static inline ssize_t oc_xattr_pfx(char *b, size_t n, const char *name) {
    int k = snprintf(b, n, "user.%s", name);
    return (k > 0 && (size_t)k < n) ? k : -1;
}
static inline ssize_t oc_getxattr(const char *p_, const char *nm, void *v, size_t s, int opt) {
    char b[256]; if (oc_xattr_pfx(b, sizeof b, nm) < 0) { errno = ERANGE; return -1; }
    return (opt & XATTR_NOFOLLOW) ? lgetxattr(p_, b, v, s) : getxattr(p_, b, v, s);
}
static inline int oc_setxattr(const char *p_, const char *nm, const void *v, size_t s, int opt) {
    char b[256]; if (oc_xattr_pfx(b, sizeof b, nm) < 0) { errno = ERANGE; return -1; }
    return (opt & XATTR_NOFOLLOW) ? lsetxattr(p_, b, v, s, 0) : setxattr(p_, b, v, s, 0);
}
static inline int oc_removexattr(const char *p_, const char *nm, int opt) {
    char b[256]; if (oc_xattr_pfx(b, sizeof b, nm) < 0) { errno = ERANGE; return -1; }
    return (opt & XATTR_NOFOLLOW) ? lremovexattr(p_, b) : removexattr(p_, b);
}
#define getxattr(p_,nm,v,s,pos,opt) oc_getxattr(p_,nm,v,s,opt)
#define setxattr(p_,nm,v,s,pos,opt) oc_setxattr(p_,nm,v,s,opt)
#define removexattr(p_,nm,opt)      oc_removexattr(p_,nm,opt)

// ---- malloc 统计（diag.c 的 L3MEM 行；OHOS 上取 0，仅诊断）----
typedef struct { size_t size_in_use; } malloc_statistics_t;
static inline void malloc_zone_statistics(void *z, malloc_statistics_t *out) { (void)z; out->size_in_use = 0; }
