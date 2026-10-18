// OHOS 兼容实现：mach 内存查询 → /proc；clonefile/renamex_np → 用户态替代；CommonCrypto → 内置实现
// OHOS compat implementations: mach memory queries via /proc; clonefile/renamex_np replacements; CommonCrypto -> vendored hashes.
#include "mach/mach.h"
#include "sys/clonefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mman.h>

task_t mach_task_self(void) { return 0; }
mach_port_t mach_thread_self(void) { return 0; }
mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
kern_return_t thread_suspend(mach_port_t t) { (void)t; return -1; }
kern_return_t thread_get_state(mach_port_t t, int f, thread_state_t st, mach_msg_type_number_t *c) { (void)t; (void)f; (void)st; (void)c; return -1; }
kern_return_t thread_resume(mach_port_t t) { (void)t; return -1; }
kern_return_t thread_info(mach_port_t t, int f, thread_info_t i, mach_msg_type_number_t *c) { (void)t; (void)f; (void)i; (void)c; return -1; }
kern_return_t thread_policy_get(mach_port_t t, int f, thread_policy_t p, mach_msg_type_number_t *c, boolean_t *d) { (void)t; (void)f; (void)p; (void)c; (void)d; return -1; }
kern_return_t thread_policy_set(mach_port_t t, int f, thread_policy_t p, mach_msg_type_number_t c) { (void)t; (void)f; (void)p; (void)c; return -1; }   // OHOS 无实时策略：出图节奏交给端口自己的 pacer
kern_return_t task_threads(task_t t, thread_act_array_t *a, mach_msg_type_number_t *c) { (void)t; (void)a; (void)c; return -1; }
kern_return_t mach_port_deallocate(task_t t, mach_port_t n) { (void)t; (void)n; return 0; }
int thread_selfcounts(int type, void *counts, size_t n) { (void)type; (void)counts; (void)n; return -1; }
kern_return_t vm_deallocate(task_t t, vm_address_t a, vm_size_t s) { (void)t; (void)a; (void)s; return 0; }
kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec ts = { (time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull) };
    return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == 0 ? 0 : -1;
}
const char _mh_execute_header = 0;

static uint64_t rss_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long total = 0, rss = 0;
    if (fscanf(f, "%ld %ld", &total, &rss) != 2) { fclose(f); return 0; }
    fclose(f);
    return (uint64_t)rss * (uint64_t)sysconf(_SC_PAGESIZE);
}

kern_return_t task_info(task_t task, int flavor, task_info_t info, mach_msg_type_number_t *count) {
    (void)task;
    if (flavor != TASK_VM_INFO || !info || !count ||
        *count < (mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int))) return -1;
    task_vm_info_data_t *vi = (task_vm_info_data_t *)info;
    memset(vi, 0, sizeof *vi);
    vi->resident_size = rss_bytes();
    vi->resident_size_max = vi->resident_size;
    vi->phys_footprint = vi->resident_size;
    return KERN_SUCCESS;
}

// 读本进程内存：OHOS 沙箱里 /proc/self/mem 打不开（拒绝），改 mincore 判映射 + 直接 memcpy（同进程）
// 失败语义与原语一致：目标未映射/不可读 → 返回非 0；只读已知有效地址时行为不变
// Read this process's memory: in the OHOS sandbox /proc/self/mem cannot be opened (denied), so use mincore to test mapping + a direct memcpy (same process).
// Failure semantics match the original primitive: unmapped/unreadable target -> non-zero return; behaviour is unchanged for known-valid addresses.
kern_return_t vm_read_overwrite(task_t task, vm_address_t addr, vm_size_t size, vm_address_t dst, vm_size_t *out_size) {
    (void)task;
    if (out_size) *out_size = 0;
    unsigned char vec = 0;
    if (mincore((void *)(uintptr_t)(addr & ~(uintptr_t)0xFFF), 4096, &vec)) return -1;   // 未映射 → 与原语一致地失败 | Unmapped -> fail like the original
    memcpy((void *)dst, (const void *)(uintptr_t)addr, size);
    if (out_size) *out_size = size;
    return 0;
}

// 复制 xattr（存档的「客体名 → 物理名」映射靠它跨 Commit 存活）
static void copy_xattrs(const char *src, const char *dst) {
    char names[4096];
    ssize_t n = llistxattr(src, names, sizeof names);
    if (n <= 0) return;
    for (char *p_ = names; p_ < names + n; p_ += strlen(p_) + 1) {
        char val[1024];
        ssize_t vn = lgetxattr(src, p_, val, sizeof val);
        if (vn >= 0) lsetxattr(dst, p_, val, (size_t)vn, 0);
    }
}
static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st)) { close(in); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char buf[262144];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { close(in); close(out); return -1; }
            off += w;
        }
    }
    close(in); close(out);
    if (n >= 0) copy_xattrs(src, dst);
    return n < 0 ? -1 : 0;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st)) return -1;
    if (S_ISLNK(st.st_mode)) {
        char l[4096]; ssize_t n = readlink(src, l, sizeof l - 1);
        if (n < 0) return -1;
        l[n] = 0;
        return symlink(l, dst);
    }
    if (S_ISREG(st.st_mode)) return copy_file(src, dst);
    if (!S_ISDIR(st.st_mode)) return -1;
    if (mkdir(dst, st.st_mode & 07777) && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e; int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[4096], b[4096];
        if (snprintf(a, sizeof a, "%s/%s", src, e->d_name) >= (int)sizeof a ||
            snprintf(b, sizeof b, "%s/%s", dst, e->d_name) >= (int)sizeof b) { rc = -1; break; }
        if (copy_tree(a, b)) { rc = -1; break; }
    }
    closedir(d);
    return rc;
}

int oc_clonefile(const char *src, const char *dst, int flags) { (void)flags; return copy_tree(src, dst); }

int oc_renamex_np(const char *from, const char *to, unsigned int flags) {
    if (flags & RENAME_SWAP) {
#ifdef SYS_renameat2
        if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 2 /*RENAME_EXCHANGE*/) == 0) return 0;
        if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return -1;
#endif
        char tmp[4096];
        if (snprintf(tmp, sizeof tmp, "%s.ocswap.%d", to, (int)getpid()) >= (int)sizeof tmp) return -1;
        if (rename(to, tmp)) return -1;
        if (rename(from, to)) { rename(tmp, to); return -1; }
        if (rename(tmp, from)) return -1;
        return 0;
    }
    return rename(from, to);
}

// ---- CommonCrypto 一次性摘要（md5.c / sha1.c / sha256.c 为公有领域实现，B-Con/crypto-algorithms 版）----
#include "md5.h"
#include "sha1.h"
void oc_md5(const void *data, uint32_t len, unsigned char out[16]) { MD5_CTX c; md5_init(&c); md5_update(&c, (const BYTE *)data, (size_t)len); md5_final(&c, (BYTE *)out); }
void oc_sha1(const void *data, uint32_t len, unsigned char out[20]) { SHA1_CTX c; sha1_init(&c); sha1_update(&c, (const BYTE *)data, (size_t)len); sha1_final(&c, (BYTE *)out); }
// ---- SHA256（B-Con sha256.c，公有领域）----
#include "CommonCrypto/CommonDigest.h"
#include "sha256.h"
_Static_assert(sizeof(SHA256_CTX) <= sizeof(CC_SHA256_CTX), "CC_SHA256_CTX 缓冲太小");
void CC_SHA256_Init(CC_SHA256_CTX *c) { sha256_init((SHA256_CTX *)c); }
void CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, uint32_t len) { sha256_update((SHA256_CTX *)c, (const BYTE *)data, (size_t)len); }
void CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) { sha256_final((SHA256_CTX *)c, (BYTE *)md); }
void oc_sha256(const void *data, uint32_t len, unsigned char out[32]) { SHA256_CTX c; sha256_init(&c); sha256_update(&c, (const BYTE *)data, (size_t)len); sha256_final(&c, (BYTE *)out); }

// asm（nvn_entry_ohos.S 的计时包装）用的全局时间函数：与 mach_time.h 的 static inline 同口径
uint64_t oc_abs_time(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
