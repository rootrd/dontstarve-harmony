#include <TargetConditionals.h>
// 崩溃现场：打印出错地址、pc、lr（换成模块内偏移）与寄存器；缺页钩子分发。
// 只放信号处理本身：诊断功能（探针 / UI 消息 / 采样）在 diag.c，本文件不依赖它们（图形单测只链本文件）。
// Crash state: prints the faulting address, pc, lr (converted to module-relative offsets) and the registers; dispatches page fault hooks.
// Only signal handling itself lives here: diagnostic features (probes / UI messages / sampling) are in diag.c, and this file does not depend on them (the graphics unit tests link only this file).
#include "dm.h"
#include <signal.h>
#include <unwind.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>

static _Atomic uint64_t g_watch_last_ns;
static _Atomic uint64_t g_watch_count;
static _Atomic uint32_t g_watch_tick;
static _Atomic int g_watch_pregame = 1;
static _Atomic int g_watch_stage_idx;
static char g_watch_stage[2][256] = {"启动", "启动"};
static unsigned g_watch_stall;
static void (*g_dump_hook)(void);
static void (*g_wait_dump)(void);

static uint64_t watch_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t crash_watch_progress_count(void) { return atomic_load(&g_watch_count); }
void crash_watch_progress(uint32_t tick, const char *stage) {
    int next = 1 - atomic_load(&g_watch_stage_idx);
    if (stage && *stage) snprintf(g_watch_stage[next], sizeof g_watch_stage[next], "%s", stage);
    atomic_store(&g_watch_tick, tick);
    atomic_store(&g_watch_stage_idx, next);
    atomic_fetch_add(&g_watch_count, 1);
    atomic_store(&g_watch_last_ns, watch_now_ns());
}
void crash_watch_frame(void) { if (atomic_load(&g_watch_pregame)) crash_watch_progress(0, "出图帧"); }
void crash_watch_pregame_done(void) { atomic_store(&g_watch_pregame, 0); }
void crash_watch_pregame_again(void) { atomic_store(&g_watch_pregame, 1); }   // 从局里退回前端：重新按出图帧算进展（diag.c） | Back from a run to the front end: count progress by presented frames again (diag.c)
void crash_dump_now(const char *why);
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        usleep(250000);
        // 先读最后进展时刻、再读现在：反过来读时，两次读之间游戏线程刚好记了一次进展，last 会比 now 新，now − last 无符号下溢成「第 18446744073 秒起无进展」，
        // 当场误判卡死退出（accept inv_drag_out，本机负载 11、看门狗线程两次读之间被切走）。这样读 last ≤ now；万一时钟回绕也不判
        // Read the last progress time first, then read now: reading in the opposite order, if the game thread happens to record progress between the two reads, last is newer than now and now − last underflows as unsigned into 「第 18446744073 秒起无进展」(no progress since second 18446744073),
        // which is misjudged on the spot as a hang and exits (accept inv_drag_out, local load 11, the watchdog thread was switched out between the two reads). Reading this way gives last ≤ now; and if the clock ever wraps around it is not judged either
        uint64_t last = atomic_load(&g_watch_last_ns), now = watch_now_ns();
        if (last > now) continue;
        // 秒级卡顿（进展停了 1 秒以上、还没到判死的阈值）：不退出，把各线程在等什么导一份。撞到「每 2 秒才出一帧」的现场，15 秒阈值抓不到、日志里没有它在等什么。
        // 进局之后才导；每个进程最多 3 份、两份之间至少隔 10 秒（读取画面、暂停菜单也会停拍，别让它们刷屏）
        // Second-scale stall (progress has stopped for more than 1 second but has not yet reached the kill threshold): do not exit, dump what each thread is waiting on. When hitting a case of "only one frame every 2 seconds", the 15-second threshold cannot catch it and the log has nothing on what it is waiting for.
        // Dump only after entering a run; at most 3 dumps per process, at least 10 seconds between two dumps (the loading screen and the pause menu also stop ticks, do not let them flood the log)
        { static int dumps; static uint64_t last_dump, dumped_for;
          if (g_watch_stall && !atomic_load(&g_watch_pregame) && atomic_load(&g_watch_tick) > 0 && now - last >= 1000000000ull && now - last < (uint64_t)g_watch_stall * 1000000000ull
              && dumps < 3 && dumped_for != last && (!last_dump || now - last_dump >= 10000000000ull)) { dumps++; last_dump = now; dumped_for = last; crash_dump_now("进展停了 1 秒以上"); } }
        if (g_watch_stall && now - last >= (uint64_t)g_watch_stall * 1000000000ull) {
            uint64_t sec = (now - last) / 1000000000ull;
            int idx = atomic_load(&g_watch_stage_idx);
            LOG("【红】看门狗：第 %llu 秒起无进展，卡在 拍 %u / 阶段 %s", (unsigned long long)sec,
                atomic_load(&g_watch_tick), g_watch_stage[idx]);
            if (g_dump_hook) g_dump_hook();
            if (g_wait_dump) g_wait_dump();   // 图形侧所有阻塞等待点的状态（nvn_mtl.m 初始化时注册） | State of all blocking wait points on the graphics side (registered when nvn_mtl.m initializes)
#if TARGET_OS_OSX
            { const char *d = getenv("DM_WATCH_SAMPLE");   // Mac 诊断：卡住时对自己采样 1 秒，线程栈写到该路径 | Mac diagnostics: when stuck, sample itself for 1 second and write the thread stacks to this path
              if (d) { char c[1200]; snprintf(c, sizeof c, "/usr/bin/sample %d 1 -f '%s' >/dev/null 2>&1", (int)getpid(), d); if (system(c)) LOG("看门狗：采样失败"); else LOG("看门狗：线程栈已写 %s", d); } }
#endif
            _exit(128 + SIGALRM);
        }
    }
    return NULL;
}
// 桩页归属：hle_count.c 封页时登记（图形单测只链本文件，不能直接引用它的符号）
// Stub-page ownership: registered by hle_count.c at seal time (the graphics unit test links only this file, so it must not reference its symbols)
static const char *(*g_page_tag)(const void *);
void crash_set_page_tag(const char *(*fn)(const void *)) { g_page_tag = fn; }
void crash_off(const char *tag, uint64_t a) {   // diag.c 也用 | Also used by diag.c
#define off crash_off

    module_t *m = module_of((void *)a);
    Dl_info di;
    const char *pt = NULL;
    if (m) {
        fprintf(stderr, " %s%s%s+%#llx", tag, *tag ? "=" : "", m->name, a - (uint64_t)m->base);
        if (!module_addr_exec((void *)(uintptr_t)a)) fprintf(stderr, "〔非代码段〕");   // 跳进数据 = 取指错误，一眼分开 | a jump into data = an instruction-fetch fault, separated at a glance
        const char *sn = module_sym_at(m, a);
        if (sn) fprintf(stderr, "〈%s〉", sn);
    }
    else if (g_page_tag && (pt = g_page_tag((void *)(uintptr_t)a))) fprintf(stderr, " %s%s=%s", tag, *tag ? "=" : "", pt);
    else if (dladdr((void *)a, &di) && di.dli_fname) {
        // dladdr 只认导出符号：宿主里静态函数（比如我们自己的等待/搬运路径）sname 为空，以前就直接甩一个裸地址，
        // 「十几条线程全停在同一个 pc」那种现场因此谁也认不出来。现在退一步打「库名+库内偏移」，
        // 拿未剥离的那份 .so 用 nm 离线就能还原成函数名。
        // dladdr only knows exported symbols: for a static function in the host (our own wait/copy paths) dli_sname is null,
        // and the old code printed a bare address - which is why a scene like "a dozen threads all parked at one pc" named
        // nothing. Fall back to library + file offset; nm against the unstripped .so resolves it offline.
        const char *bn = strrchr(di.dli_fname, '/'); bn = bn ? bn + 1 : di.dli_fname;
        if (di.dli_sname) fprintf(stderr, " %s%s%s+%#lx〈%s〉", tag, *tag ? "=" : "", di.dli_sname,
                                  (unsigned long)(a - (uint64_t)di.dli_saddr), bn);
        else fprintf(stderr, " %s%s%s+%#lx〈无符号〉", tag, *tag ? "=" : "", bn,
                     (unsigned long)(a - (uint64_t)di.dli_fbase));
    }
    else fprintf(stderr, " %s%s%#llx", tag, *tag ? "=" : "", a);
}
// 帧指针回溯：x29 链 [fp]=上一帧 fp、[fp+8]=返回地址；只走 8 字节对齐、单调上升的帧
// Frame pointer backtrace: x29 chain [fp]=previous frame's fp, [fp+8]=return address; only 8-byte aligned, monotonically increasing frames are walked
static size_t rd_at(uint64_t a, void *out, size_t n);   // 可读区间表取数（下面定义）| read through the readable-range table (defined below)
void dm_backtrace(uint64_t fp) {
    fprintf(stderr, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t fr[2];
        if (rd_at(fp, fr, sizeof fr) != sizeof fr) break;
        off("", fr[1]);
        if (fr[0] <= fp) break;
        fp = fr[0];
    }
    fputc('\n', stderr);
}
// ---------- 缺页钩子（写保护脏页跟踪） ----------
// on_crash 打印现场前按注册顺序调用；返回 1 = 已处理（直接返回、重执行出错指令），全部返回 0 才走崩溃路径。
// 钩子运行在信号处理上下文：只许查表 / 原子操作 / mprotect，不许分配内存、加锁。
// ---------- Page fault hooks (write-protect dirty page tracking) ----------
// Called in registration order before on_crash prints the crash state; returning 1 = handled (return directly and re-execute the faulting instruction); the crash path is taken only when all return 0.
// Hooks run in signal handler context: only table lookups / atomic operations / mprotect are allowed, no memory allocation and no locking.
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static void (*g_write_prep[MAX_FAULT_HOOK])(void *addr, size_t len);
static int g_nfault_hook, g_nwrite_prep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    if (g_nfault_hook >= MAX_FAULT_HOOK) die("缺页钩子过多");
    g_fault_hook[g_nfault_hook++] = fn;
}
static void (*g_res_report)(uint32_t tick, const char *why);   // 资源计数回调（图形层注册，diag.c 按拍 / 事件调用） | Resource count callback (registered by the graphics layer, called by diag.c per tick / event)
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why)) { g_res_report = fn; }
void (*dm_res_report_cb(void))(uint32_t, const char *) { return g_res_report; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    if (g_nwrite_prep >= MAX_FAULT_HOOK) die("宿主写前钩子过多");
    g_write_prep[g_nwrite_prep++] = fn;
}
// 宿主自己写客体内存（read() 直读进客体缓冲等）之前调用：写保护页不会对系统调用触发缺页、而是返回 EFAULT
// Called before the host itself writes guest memory (read() straight into a guest buffer, etc.): for a system call a write-protected page does not trigger a page fault but returns EFAULT
void dm_guest_write_prepare(void *addr, size_t len) { for (int i = 0; i < g_nwrite_prep; i++) g_write_prep[i](addr, len); }

// 诊断模块（diag.c 的探针）与线程现场（hle_thread.c）由各自注册进来：单测只链本文件时不依赖它们
// The diagnostics module (the probes in diag.c) and the thread state dump (hle_thread.c) each register themselves here: a unit test that links only this file does not depend on them
static int (*g_trap_hook)(ucontext_t *uc);
void crash_set_trap_hook(int (*fn)(ucontext_t *uc)) { g_trap_hook = fn; }
void crash_set_dump_hook(void (*fn)(void)) { g_dump_hook = fn; }
void crash_set_wait_dump(void (*fn)(void)) { g_wait_dump = fn; }
// 「崩的这条是客体哪个线程」由 hle_thread.c 注册进来（它才认识 thr_t / SDK 线程名 / 线程函数）
// hle_thread.c registers how to say which guest thread crashed (only it knows thr_t / the SDK thread name / the thread function)
static void (*g_thr_desc)(char *, size_t);
void crash_set_thr_desc(void (*fn)(char *, size_t)) { g_thr_desc = fn; }
// 按需导出现场（不退出）：iOS 包没有无进展看门狗，冒烟判红时经 DM_LOG_PORT 发 dump 触发——各客体线程在等什么 + 图形侧等待点，和看门狗打的是同一份
// Dump the current state on demand (without exiting): the iOS app has no no-progress watchdog, so when the smoke test fails it sends dump through DM_LOG_PORT to trigger this -- what each guest thread is waiting on + the graphics-side wait points, the same thing the watchdog prints
void crash_dump_now(const char *why) { fprintf(stderr, "[dm] 现场导出（%s）：拍 %u / 阶段 %s\n", why, atomic_load(&g_watch_tick), g_watch_stage[atomic_load(&g_watch_stage_idx)]); if (g_dump_hook) g_dump_hook(); if (g_wait_dump) g_wait_dump(); }
// 宿回溯：x29 链在宿主里也常断，用 _Unwind_Backtrace 打出宿主调用链（符号用 dladdr 解）
// Host backtrace: the x29 chain breaks in host code too, so use _Unwind_Backtrace and symbolize with dladdr
static _Unwind_Reason_Code unw_cb(struct _Unwind_Context *ctx, void *arg) {
    (void)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) { Dl_info di;
        if (dladdr((void *)pc, &di) && di.dli_sname) fprintf(stderr, " %s+%#lx", di.dli_sname, (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        else fprintf(stderr, " %#lx", (unsigned long)pc);
    }
    return _URC_NO_REASON;
}
static void crash_unwind(void) { fprintf(stderr, "[dm] 宿回溯:"); _Unwind_Backtrace(unw_cb, 0); fputc('\n', stderr); }

// 现场对象翻查：对若干寄存器指向的内存，按 8 字节字段扫 0x100 字节，字段本身是指向可打印串的指针就报出来（带偏移）。
// 用来回答「这个对象到底是空的还是半构造的」「它里面那条错误字符串写了什么」。
//
// 可读性判据必须是「这条区间能读」，不是「这页映射了」：mincore 把 PROT_NONE 的页也算 mapped，拿它当判据直接解引用，
// 就是在处理器里再崩一次 —— v26 实测：crash_65046.txt 止于寄存器那份，回溯/调用环/NVN 计数/栈窗一行都没有
// （客体刚把 x1=0x6b82000000 这种保留地址交进来）。判据改成开一次 /proc/self/maps 建可读区间表，探针只按表取数。
// The test must be "this range is readable", not "this page is mapped": mincore counts PROT_NONE pages as mapped, so the old
// guard let the handler fault on its own probe and the whole scene after the registers was lost (measured in v26 -
// crash_65046.txt stops at the register dump, no backtrace / call rings / NVN census / stack window). Build a readable-range
// table from /proc/self/maps once and read only through it.
#define MR_MAX 65536
typedef struct { uint64_t s, e; } mr_t;
static mr_t g_mr[MR_MAX];
static int g_nmr = -1;      // -1 = 还没建表 | table not built yet

static uint64_t mr_hex(const char **pp) {
    uint64_t v = 0;
    for (;;) {
        char c = **pp; int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return v;
        v = v * 16u + (unsigned)d; (*pp)++;
    }
}
static void mr_line(const char *l) {
    const char *p = l; uint64_t s = mr_hex(&p);
    if (*p != '-' || g_nmr >= MR_MAX) return;
    p++; uint64_t e = mr_hex(&p);
    if (*p != ' ' || s >= e) return;
    if (p[1] != 'r') return;
    if (g_nmr && g_mr[g_nmr - 1].e == s) { g_mr[g_nmr - 1].e = e; return; }   // 与上一条相接就并进去 | merge with the previous range
    g_mr[g_nmr++] = (mr_t){ s, e };
}
// 读一次映射表：信号上下文只用 open/read/close，不分配内存；崩现场在打探针前强制刷一次，其余调用方只在第一次建。
// 相邻的可读区间当场合并（客体的按页写保护会把映射切成几千段，实测 4096 条上限连客体栈都覆盖不到），查表用二分。
// Build the table with open/read/close only (no allocation): the crash path forces a refresh before probing, other callers
// build once. Adjacent readable ranges are merged while reading (per-page write protection slices the mappings into thousands
// of entries - measured: the 4096-entry cap could not even cover the guest stack), and lookups are a binary search.
static void maps_load(int force) {
    if (g_nmr >= 0 && !force) return;
    g_nmr = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;                       // iOS/macOS 没有 /proc：表空 => 下面的探针退回旧判据 | no /proc there: empty table = old fallback below
    static char chunk[8192]; char line[512]; size_t ln = 0;
    for (;;) {
        ssize_t k = read(fd, chunk, sizeof chunk);
        if (k <= 0) break;
        for (ssize_t i = 0; i < k; i++) {
            char c = chunk[i];
            if (c == '\n') { line[ln] = 0; mr_line(line); ln = 0; }
            else if (ln < sizeof line - 1) line[ln++] = c;   // 超长行截前 511 字节：地址段就在行首 | over-long line: the address field is at the head
        }
    }
    close(fd);
    if (ln) { line[ln] = 0; mr_line(line); }
}
static int obj_page_mapped(uint64_t a) { unsigned char m; return !mincore((void *)(uintptr_t)(a & ~0xFFFull), 4096, &m); }
// 从 a 起最多拷 n 字节，返回真拷到的字节数（0 = 读不到）。表在用时只认表里的可读区间；没有表（iOS）按页内安全量拷。
// Copy at most n bytes from a and return how many were actually readable (0 = nothing). With the table only its ranges count;
// without one (iOS) copy within the page, matching the old behaviour.
static size_t rd_at(uint64_t a, void *out, size_t n) {
    if (a < 0x1000 || n == 0) return 0;
    maps_load(0);
    if (g_nmr > 0) {
        int lo = 0, hi = g_nmr - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (a < g_mr[mid].s) hi = mid - 1;
            else if (a >= g_mr[mid].e) lo = mid + 1;
            else {
                size_t avail = (size_t)(g_mr[mid].e - a);
                if (avail > n) avail = n;
                memcpy(out, (const void *)(uintptr_t)a, avail);
                return avail;
            }
        }
        return 0;
    }
    if (!obj_page_mapped(a)) return 0;
    size_t avail = 0x1000 - (size_t)(a & 0xFFF);
    if (avail > n) avail = n;
    memcpy(out, (const void *)(uintptr_t)a, avail);
    return avail;
}
// 把一个地址说成人话：客体模块里 → 「模块名+偏移」；宿主 .so 里 → dladdr 的符号名；其余原样。
// 用来回答「客体全局里存的那个函数指针，到底是宿主的哪一个实现」——跳板符号是全局的，剥了符号表的包里 dladdr 仍给得出 _nvnth_<i>。
// Human-readable form of an address: guest module -> "name+offset"; host .so -> dladdr symbol; otherwise raw.
// Answers "which host implementation does that pointer in the guest's global actually dispatch to": the thunks are global
// symbols, so dladdr still names them _nvnth_<i> in the stripped packaged .so.
static void sym_of(uint64_t a, char *out, size_t n) {
    module_t *m = module_of((void *)(uintptr_t)a);
    if (m && a >= (uint64_t)m->base) { snprintf(out, n, "%s+%#llx", m->name, (unsigned long long)(a - (uint64_t)m->base)); return; }
    Dl_info di;
    if (a >= 0x1000 && dladdr((void *)(uintptr_t)a, &di) && di.dli_sname) {
        snprintf(out, n, "宿主 %s+%#lx", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr)); return;
    }
    snprintf(out, n, "%#llx", (unsigned long long)a);
}
// 客体的「取全局 → 再取首字 → blr」三件套：把 pc 附近每条 adrp+ldr 指到的全局读出来，连它首字一起解析成符号。
// 崩在解引用上、而调用链要经由客体自己的派发全局时，这两行就说清了「到底调的是宿主的哪个 NVN 实现」。
// The guest's "adrp -> ldr global -> ldr first word -> blr" triple: read every global an adrp+ldr pair near pc points at
// and resolve both it and its first word. When the call chain goes through the guest's own dispatch globals, this names
// exactly which host NVN implementation was called.
static void globals_near_pc(uint64_t pc) {
    char gs[64], gf[64];
    uint32_t w[256];
    uint64_t lo = (pc & ~3ull) - 0x200;
    if (rd_at(lo, w, sizeof w) != sizeof w) return;
    uint64_t page[32];
    for (int r = 0; r < 32; r++) page[r] = 0;
    for (unsigned k = 0; k < sizeof w / sizeof w[0]; k++) {
        uint64_t a = lo + k * 4;
        uint32_t i = w[k];
        if ((i & 0x9F000000u) == 0x90000000u) {          // adrp xd, #imm
            int64_t immlo = (i >> 29) & 3, immhi = (i >> 5) & 0x7FFFF;
            int64_t im = (immhi << 2) | immlo;
            if (im & (1ll << 20)) im |= ~(0x1FFFFFll);   // 符号扩展 21 位 | sign-extend 21 bits
            page[i & 31] = (a & ~0xFFFull) + (im << 12);
            continue;
        }
        if ((i & 0xFFC00000u) == 0xF9400000u) {          // ldr rt, [rn, #imm<<3]
            unsigned rn = (i >> 5) & 31, rt = i & 31;
            uint64_t g = page[rn] ? page[rn] + ((i >> 10) & 0xFFF) * 8 : 0;
            uint64_t v = 0, f = 0;
            if (g >= 0x1000 && rd_at(g, &v, 8) == 8) {
                sym_of(g, gs, sizeof gs);
                if (v >= 0x1000 && !(v & 7) && rd_at(v, &f, 8) == 8 && f >= 0x1000) {
                    sym_of(f, gf, sizeof gf);
                    fprintf(stderr, "[dm]   派发 %s = %#llx → 首字 %s\n", gs, (unsigned long long)v, gf);
                } else fprintf(stderr, "[dm]   全局 %s = %#llx\n", gs, (unsigned long long)v);
            }
            page[rt] = 0;                                 // 目标寄存器被改写，页号作废 | the register no longer holds a page
        }
    }
}
static size_t obj_str_at(uint64_t v, char *buf, size_t cap) {
    if (v < 0x1000 || (v & 7)) return 0;
    size_t room = rd_at(v, buf, cap - 1);
    if (room < 6) return 0;
    size_t n = 0;
    while (n < room && buf[n] >= 0x20 && buf[n] < 0x7f) n++;
    if (n >= 5 && n < room && buf[n] == 0) { buf[n] = 0; return n; }
    return 0;
}
static void crash_dump_objects(ucontext_t *uc) {
    static const int rr[] = {19, 20, 21, 22, 23, 26, 8, 0, 1, 3};
    char sb[80];
    for (unsigned q = 0; q < sizeof rr / sizeof rr[0]; q++) {
        uint64_t base = OC_SS_X(uc, rr[q]);
        if (base < 0x1000 || (base & 7)) continue;
        int shown = 0;
        for (uint64_t off = 0; off < 0x100; off += 8) {
            uint64_t v = 0;
            if (rd_at(base + off, &v, 8) != 8) break;
            size_t n = obj_str_at(v, sb, sizeof sb);
            if (n) { fprintf(stderr, "[dm]   x%d+%#llx → \"%s\"\n", rr[q], (unsigned long long)off, sb); if (++shown == 8) break; }
        }
    }
}

static volatile int g_in_crash;
// 崩现场直接落盘：stderr 在 OHOS 上走「管道 → hilog 桥」，客体刷屏时桥会丢行——第 132 轮实测崩前三行
// （CRASH sig/addr/pc/lr）一条都没到 hilog，只到了它后面那份计数。现场不能赌日志通道：崩的第一件事把 fd 2 换到文件。
// The crash report goes straight to a file: on OHOS stderr is a pipe drained by a hilog bridge, which loses lines while the
// guest floods - round 132 measured every header line (CRASH sig/addr/pc/lr) missing while the counter dump right after it
// arrived. So the first thing the handler does is point fd 2 at a file. Path: DM_CRASHDIR, else DM_SAVE.
static void crash_fd_to_file(int sig, siginfo_t *si, ucontext_t *uc)
{
    const char *d = getenv("DM_CRASHDIR");
    if (!d || !*d) d = getenv("DM_SAVE");
    if (!d || !*d) return;
    char p[512];
    snprintf(p, sizeof p, "%s/crash_%u.txt", d, (unsigned)getpid());
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    dup2(fd, 2);
    setvbuf(stderr, NULL, _IONBF, 0);   // 先把桥里攒着的行倒进文件，再改成无缓冲：后面的现场一行都不许停在缓冲里
    // 头一行用 write 直写：fprintf 那条路在实测里丢过头半截（"CRASH sig/addr" 没了，只剩 " pc=..."）
    char h[320];
    int n = snprintf(h, sizeof h, "[dm] CRASH sig=%d addr=%#llx pc=%#llx lr=%#llx sp=%#llx\n",
                     sig, (unsigned long long)(uintptr_t)si->si_addr,
                     (unsigned long long)OC_SS_PC(uc), (unsigned long long)OC_SS_LR(uc), (unsigned long long)OC_SS_SP(uc));
    if (n > 0) { ssize_t w = write(2, h, (size_t)n); (void)w; }
    close(fd);
}
static void on_crash(int sig, siginfo_t *si, void *uc_) {

    if (g_in_crash) { fflush(stderr); _exit(9); } g_in_crash = 1;    ucontext_t *uc = uc_;
    // 陷阱钩子先跑：客体 BRK 是常态（SIGTRAP 被钩子接走后进程继续跑），不能因为它把 stderr 永久指向崩溃文件
    // The trap hook goes first: guest BRKs are routine (the hook takes the SIGTRAP and the process carries on), so it must not
    // leave stderr pointed at the crash file.
    if (sig == SIGTRAP && g_trap_hook && g_trap_hook(uc)) { g_in_crash = 0; return; }
    if (sig == SIGSEGV || sig == SIGBUS) {
        // ESR 的 WnR 位 = 写。OHOS/Linux 的 ucontext 不带 ESR（OC_ES_ESR 恒 0），照原样判会把每次缺页当成「读」直接报崩
        // （B49 实测：贴图池页设了只读，游戏一写就 CRASH sig=11，钩子一次都没接手）。
        // 兜底认法：SEGV_ACCERR = 映射在、权限不对，而脏页跟踪的页始终可读（只读 = 只有 PROT_READ），所以权限错只能是写。
        // si_addr == pc 是取指错（跳进不可执行页），当写会让那页解保护后无限重执行，排除掉。
        // 认错也无害：钩子还要查页状态，没保护过的页（代号为 0）照样返回 0，走如实报崩。
        // The WnR bit of ESR means "write". OHOS/Linux ucontext carries no ESR (OC_ES_ESR is always 0), so the old test read every fault as a "read" and reported a crash
        // (measured in B49: texture pool pages set read-only, the game's first write gave CRASH sig=11 and the hook never took it).
        // Fallback: SEGV_ACCERR means "mapped but wrong permission", and dirty-tracked pages are always readable (read-only = PROT_READ only), so a permission fault can only be a write.
        // si_addr == pc is an instruction fetch (jumped into a non-executable page); treating it as a write would unprotect that page and re-execute forever, so exclude it.
        // A wrong guess is still safe: the hook also checks the page state, and a never-protected page (generation 0) returns 0 and the crash is reported truthfully.
        uint64_t esr = OC_ES_ESR(uc);
        int is_write = esr ? (int)((esr >> 6) & 1)
                           : (si->si_code == SEGV_ACCERR && (uint64_t)(uintptr_t)si->si_addr != OC_SS_PC(uc));
        // 钩子处理掉了必须清重入位：不清的话下一次缺页被当成「在处理器里又崩」，直接 _exit(9) 无声掉进程
        // When a hook handled the fault the re-entry flag must be cleared, otherwise the next fault looks like "crashed inside the handler" and _exit(9) kills the process silently
        for (int i = 0; i < g_nfault_hook; i++) if (g_fault_hook[i](si->si_addr, is_write, uc)) { g_in_crash = 0; return; }
    }
    crash_fd_to_file(sig, si, uc);
    maps_load(1);   // 建/刷可读区间表：现场探针全靠它，处理器里绝不碰表外的地址 | build the readable-range table; no probe touches an address outside it
    // 表为空 = /proc/self/maps 没读到，探针只能走旧的按页判据（那种页会算 mapped，风险还在）：这条要说出来，
    // 否则「下面几段没打印」会被当成「客体没走到」
    // An empty table means /proc/self/maps could not be read and the probes fall back to the old per-page test (which counts
    // PROT_NONE as mapped, so the risk is back): say so, otherwise "no lines below" would be read as "the guest got no further".
    fprintf(stderr, "[dm] 可读区间 %d 条（现场探针只按表内地址取数）\n", g_nmr);
    fprintf(stderr, "[dm] CRASH sig=%d", sig);
    off("addr", (uint64_t)si->si_addr); off("pc", OC_SS_PC(uc)); off("lr", OC_SS_LR(uc));
    // pc 处三条指令的机器码：用来核对 pc→模块偏移有没有错位（本例 addr=0 但按反汇编前一条也该炸，得确认取到的 pc 真是那条）
    { uint64_t pp = OC_SS_PC(uc); uint32_t w[3] = {0};
      if (module_of((void *)(uintptr_t)pp) && rd_at(pp - 8, w, sizeof w) == sizeof w)
          fprintf(stderr, " 指令=%08x %08x [%08x]", w[0], w[1], w[2]);
    }
    fprintf(stderr, " sp=%#llx", OC_SS_SP(uc));
    { char td[200] = ""; if (g_thr_desc) g_thr_desc(td, sizeof td); fprintf(stderr, " 线程=%s", td); }   // 崩的是哪条客体线程（槽/名/线程函数）| which guest thread crashed
    fprintf(stderr, "\n");
    fprintf(stderr, " fp=%#llx\n", OC_SS_FP(uc));
    for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%#llx%s", i, OC_SS_X(uc,i), i % 6 == 5 ? "\n" : " ");
    fprintf(stderr, "\n");
    // 必须有的取证排在前面，对象翻查/线程参数翻查排到最后：v26 那轮就是处理器自己再崩一次，
    // 把回溯、调用环、NVN 计数、栈窗一起带走了。现在探针只按可读区间表取数，但顺序本身也是保险。
    // The must-have evidence goes first and the two object scans last: in v26 the handler faulted inside itself and took the
    // backtrace, the call rings, the NVN census and the stack window down with it. Probes now only read through the
    // readable-range table, but the ordering is a second belt.
    dm_backtrace(OC_SS_FP(uc));
    { extern void hle_hist_dump(void); hle_hist_dump(); }
    { extern void hle_trace_dump(void); hle_trace_dump(); }   // 分线程的真调用序列（DM_HLTRACE；记名桩本来就在记）| per-thread real call sequence
    { extern void nvn_dump_counts(const char *); nvn_dump_counts("崩溃时"); }
    { extern void nvn_trace_dump(const char *, uint64_t); nvn_trace_dump("崩溃时", OC_SS_SP(uc)); }   // DM_NVN_TRACE：末尾 72 条按序 NVN 调用，星标 = 与崩溃栈同一 64 KB 的线程 | last 72 NVN calls in order; star = caller sp shares the crash stack's 64 KB
    { extern void hlecount_dump_now(void); hlecount_dump_now(); }   // 崩溃时"哪些剪口真被调过、各几次"：0 条 = 客体根本没走到这些接口 | Which HLE entries actually ran at crash time: no lines = the guest never reached those interfaces   // 崩溃时 NVN 各类调用计数：0 = 根本没摸到图形层 | NVN call counts at crash time: 0 = the graphics layer was never touched
    crash_unwind();
    // 客栈窗扫描：帧链常断（客体把 x29 当普通寄存器用），从 sp 起拷 2 KB，
    // 落在客体模块区间里的值基本都是返回地址 —— 用来还原崩溃前的客体调用链。
    // Guest stack-window scan: the frame chain often breaks (guest code uses x29 as a scratch register); copy 2 KB from
    // sp; values falling inside guest modules are almost always return addresses.
    {
        uint64_t sp = OC_SS_SP(uc);
        static uint64_t v[256];
        if (sp > 0x10000 && !(sp & 7)) {
            size_t n = rd_at(sp, v, sizeof v);
            fprintf(stderr, "[dm] 客栈窗:");
            for (size_t k = 0; k < n / 8; k++) {
                module_t *mm = module_of((void *)(uintptr_t)v[k]);
                if (mm && v[k] >= (uint64_t)mm->base) fprintf(stderr, " %s+%#llx", mm->name, v[k] - (uint64_t)mm->base);
            }
            fputc('\n', stderr);
        }
    }
    // 裸栈窗 + 候选对象：上面两条扫描只打「像代码」的值，局部变量/对象指针全被过滤掉了。
    // 现场见过这种组合：调用者把对象指针存进 [x29,#-8]，回来读它是 0，可它进函数时不可能是 0
    // （同一条指令流更早就按它取过数）——那只剩「被调方按出参指针写穿了自己那份结构，多出来的 0 覆盖了上面的槽」。
    // 这两段就是判这个的：整窗原样打出来，再把「首字像虚表」的字按对象翻开，连带崩掉的那个字段偏移一起读。
    // Raw window + object candidates: the two scans above drop everything that is not code-shaped. A seen pattern is: the
    // caller stores an object pointer at [x29,#-8] and reads back 0, although it cannot have been 0 on entry (the same
    // instruction stream dereferenced it earlier), which leaves only "a callee wrote past its out-struct and the extra
    // zeros covered the slot above it". These two blocks decide that: the window as-is, plus every word whose first word
    // looks like a vtable opened as an object, including the field offset that just faulted.
    {
        uint64_t sp = OC_SS_SP(uc), addr = (uint64_t)si->si_addr;
        static uint64_t rw[128];
        size_t n = sp > 0x10000 && !(sp & 7) ? rd_at(sp, rw, sizeof rw) : 0;
        for (size_t k = 0; k + 4 <= n / 8; k += 4)
            fprintf(stderr, "[dm]   栈 sp+0x%03zx = 0x%016llx 0x%016llx 0x%016llx 0x%016llx\n", k * 8,
                    (unsigned long long)rw[k], (unsigned long long)rw[k + 1], (unsigned long long)rw[k + 2], (unsigned long long)rw[k + 3]);
        uint64_t foff = addr < 0x100000 ? addr : 0;   // 基址为 0 的崩法里，错地址就是字段偏移 | with a null base the fault address is the field offset
        int shown = 0;
        for (size_t k = 0; k < n / 8 && shown < 24; k++) {
            uint64_t o = rw[k], vt = 0, f = 0;
            if (o < 0x10000 || (o & 7) || rd_at(o, &vt, 8) != 8) continue;
            module_t *mv = module_of((void *)(uintptr_t)vt);
            if (!mv || vt < (uint64_t)mv->base) continue;
            fprintf(stderr, "[dm]   候选对象 栈sp+%#zx=%#llx 虚表=%s+%#llx", k * 8, (unsigned long long)o,
                    mv->name, (unsigned long long)(vt - (uint64_t)mv->base));
            if (foff && rd_at(o + foff, &f, 8) == 8) fprintf(stderr, " [+%#llx]=%#llx", (unsigned long long)foff, (unsigned long long)f);
            if (rd_at(o + 8, &f, 8) == 8) fprintf(stderr, " [+8]=%#llx", (unsigned long long)f);
            fputc('\n', stderr); shown++;
        }
        uint64_t pc = OC_SS_PC(uc);
        if (module_of((void *)(uintptr_t)pc)) globals_near_pc(pc);
    }
   // 最近的宿主→客体调用，便于定位“错值从哪一步交出去” | recent host->guest calls, to locate which step handed out a bad value
   // 诊断：从 sp 起 8 KB，凡落入客体模块或宿主 .so 区间的字都打出来（带符号），用来还原调用链
    {
        uint64_t sp = OC_SS_SP(uc); Dl_info di; uint64_t hb = 0, he = 0;
        static uint64_t stk[1024];
        if (dladdr((void *)&on_crash, &di) && di.dli_fbase) { hb = (uint64_t)di.dli_fbase; he = hb + 0x400000; }
        fprintf(stderr, "[dm] 宿text %#llx..%#llx\n", (unsigned long long)hb, (unsigned long long)he);
        size_t n = sp > 0x10000 && !(sp & 7) ? rd_at(sp, stk, sizeof stk) : 0;
        for (size_t k = 0; k < n / 8; k++) {
            uint64_t v = stk[k];
            module_t *mm = module_of((void *)(uintptr_t)v);
            if (mm && v >= (uint64_t)mm->base)
                fprintf(stderr, "[dm]   sp+%#zx = %#llx → %s+%#llx\n", k * 8, (unsigned long long)v, mm->name, (unsigned long long)(v - (uint64_t)mm->base));
            else if (hb && v >= hb && v < he) {
                Dl_info d2;
                if (dladdr((void *)(uintptr_t)v, &d2) && d2.dli_sname)
                    fprintf(stderr, "[dm]   sp+%#zx = %#llx → 宿主 %s+%#lx\n", k * 8, (unsigned long long)v, d2.dli_sname, (unsigned long)(v - (uint64_t)d2.dli_saddr));
                else fprintf(stderr, "[dm]   sp+%#zx = %#llx → 宿主 +%#llx\n", k * 8, (unsigned long long)v, (unsigned long long)(v - hb));
            }
        }
    }
    // 客体把「错误说明」存在对象字段里、交给日志函数打印（FMOD 事件系统就是这么存的）；崩在解引用上时没人来得及打，
    // 所以我们自己按寄存器指向的对象翻一遍字段。
    // The guest keeps its "why" inside object fields and hands them to its own log function; when it faults before printing,
    // read the fields of the objects the registers point at.
    crash_dump_objects(uc);
    // 线程参数对象：空指针崩在客体线程上时，这里通常能直接看出"对象被释放/清零"（整块 0）还是"某个字段没设过"
    { extern uint64_t thr_cur_arg(void); uint64_t a = thr_cur_arg();
      fprintf(stderr, "[dm] 线程参数=%#llx\n", (unsigned long long)a);
      if (a && !(a & 7)) {
          for (int i = 0; i < 80; i++) {
              uint64_t v = 0;
              if (rd_at(a + (uint64_t)i * 8, &v, 8) != 8) { fprintf(stderr, "[dm]   参数+%#x 读不到\n", i * 8); break; }
              module_t *mm = module_of((void *)(uintptr_t)v);
              if (mm && v >= (uint64_t)mm->base)
                  fprintf(stderr, "[dm]   参数+%#x = %#llx → %s+%#llx\n", i * 8, (unsigned long long)v, mm->name,
                          (unsigned long long)(v - (uint64_t)mm->base));
              else if (v)
                  fprintf(stderr, "[dm]   参数+%#x = %#llx\n", i * 8, (unsigned long long)v);
              else if (i < 24)
                  fprintf(stderr, "[dm]   参数+%#x = 0\n", i * 8);
          }
      } }

    if (g_dump_hook) g_dump_hook();
    // stderr→hilog 桥（OHOS app_ohos.c）是另一条线程在读管道：这里立刻 _exit 会把驱动 abort 前打进 stderr 的那句话一起带走。
    // 先冲干、再等一小段让桥线程读完，才退出——驱动自报的断言文本就在这几毫秒里。
    // The stderr→hilog bridge (OHOS app_ohos.c) is another thread reading the pipe: exiting right here takes away the line the driver wrote to stderr before aborting. Flush and give the bridge a few ms to drain first.
    fflush(stderr); fflush(stdout);
    { struct timespec dr = {0, 300000000}; nanosleep(&dr, 0); }
    _exit(128 + sig);
}
// 驱动/系统库会在自己初始化时自装 SIGSEGV 处理器，把现场从我们手里抢走（实测记录见 dm/ohos/vk_smoke.c：
// 「驱动会自装信号处理器抢走现场」）。周期性复查：处理器不是我们的就补装并报红一次。
// 被抢走时进程直接被裸信号杀掉（appspawn 只报 exit with signal:11），hilog 里一行现场都没有 —— 换游戏/换驱动后尤其容易撞。
// Drivers and system libraries install their own SIGSEGV handler while initializing and take the crash scene away from us
// (see the measurement note in dm/ohos/vk_smoke.c). Re-check periodically: if the handler is not ours, reinstall it and
// report once. When it is stolen the process is killed by the raw signal (appspawn only says "exit with signal:11") and
// hilog has not a single line of the crash state -- easy to hit after switching game or driver.
static _Atomic unsigned g_handlers_stolen;
unsigned crash_handler_stolen(void) { return atomic_load(&g_handlers_stolen); }
void crash_reassert(void) {
    struct sigaction cur;
    if (sigaction(SIGSEGV, NULL, &cur)) return;
    if (cur.sa_sigaction == on_crash) return;
    unsigned n = atomic_fetch_add(&g_handlers_stolen, 1) + 1;
    if (n == 1) LOG("【红】SIGSEGV 处理器被换手（现在是 %p，不是宿主现场处理器）：补装（驱动/系统库自装处理器）", (void *)cur.sa_sigaction);
    struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
}

// 信号备用栈是每线程的：每个客体线程起跑时都要调// 信号备用栈是每线程的：每个客体线程起跑时都要调（栈溢出时才能在备用栈上打出现场）
// 备用栈随线程退出释放：pthread 键的析构在线程退出时调用（先停用备用栈再 free）。游戏会频繁起短命线程，不释放每个漏 64 KiB。
// The signal alternate stack is per thread: every guest thread must call this when it starts running (only then can the crash state be printed on the alternate stack on a stack overflow)
// The alternate stack is released when the thread exits: the pthread key destructor is called at thread exit (disable the alternate stack first, then free). The game frequently starts short-lived threads, and without releasing each one leaks 64 KiB.
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_free(void *p) { stack_t off = {.ss_flags = SS_DISABLE}; sigaltstack(&off, 0); free(p); }
static void altstack_key_init(void) { if (pthread_key_create(&g_altstack_key, altstack_free)) die("altstack key"); }
void crash_thread_init(void) {
    pthread_once(&g_altstack_once, altstack_key_init);
    if (pthread_getspecific(g_altstack_key)) return;                 // 同一线程重复调用不重复分配 | Repeated calls on the same thread do not allocate again
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    pthread_setspecific(g_altstack_key, ss.ss_sp);
}
void crash_install(void) {
    crash_thread_init();
    struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGALRM, &sa, 0);
    // 两种看门狗都只在测试显式设了 DM_WATCH / DM_WATCH_STALL 时开：产品（玩家从桌面启动，无环境变量）一个不开。
    // 读代码查到：iOS main.m 缺省 setenv DM_WATCH=30 → 玩家启动 30 s 被 SIGALRM 杀；无进展看门狗缺省 15 s → 暂停菜单（拍停）15 s 被杀
    // Both watchdogs are turned on only when a test explicitly sets DM_WATCH / DM_WATCH_STALL: the product (the player launches from the desktop, no environment variables) turns on neither.
    // Found by reading the code: iOS main.m did setenv DM_WATCH=30 by default → the player's launch was killed by SIGALRM at 30 s; the no-progress watchdog defaulted to 15 s → the pause menu (ticks stopped) was killed at 15 s
    const char *watch = getenv("DM_WATCH"), *stall = getenv("DM_WATCH_STALL");
    if (watch) alarm(atoi(watch));
    if (watch || stall) {
        g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;
        atomic_store(&g_watch_last_ns, watch_now_ns());
        pthread_t th; if (pthread_create(&th, NULL, watchdog_main, NULL)) die("看门狗线程");
        pthread_detach(th);
    }
    if (getenv("DM_CRASH_OFF")) { LOG("DM_CRASH_OFF：宿主不接管 SIGSEGV/SIGBUS/SIGILL/SIGTRAP，交给客体自己的处理器"); }   // 诊断：让客体（SDK）自己的异常处理器先跑 | diagnostics: let the guest's own handlers run
    else {
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
    }
    // 驱动/系统库常用 abort()（实测 Maleoon vkCreateSwapchainKHR 走 SIGABRT），装上才能在 hilog 里看到自己的 pc/backtrace
    sigaction(SIGABRT, &sa, 0);
}
