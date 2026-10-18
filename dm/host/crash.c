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
    else if (dladdr((void *)a, &di) && di.dli_sname) fprintf(stderr, " %s%s%s+%#lx", tag, *tag ? "=" : "", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    else fprintf(stderr, " %s%s%#llx", tag, *tag ? "=" : "", a);
}
// 帧指针回溯：x29 链 [fp]=上一帧 fp、[fp+8]=返回地址；只走 8 字节对齐、单调上升的帧
// Frame pointer backtrace: x29 chain [fp]=previous frame's fp, [fp+8]=return address; only 8-byte aligned, monotonically increasing frames are walked
void dm_backtrace(uint64_t fp) {
    fprintf(stderr, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t nfp = ((uint64_t *)fp)[0], ra = ((uint64_t *)fp)[1];
        off("", ra);
        if (nfp <= fp) break;
        fp = nfp;
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
static int obj_page_mapped(uint64_t a) { unsigned char m; return !mincore((void *)(uintptr_t)(a & ~0xFFFull), 4096, &m); }
static size_t obj_str_at(uint64_t v, char *buf, size_t cap) {
    if (v < 0x1000 || (v & 7) || !obj_page_mapped(v)) return 0;
    const unsigned char *pb = (const unsigned char *)(uintptr_t)v;
    size_t room = 0x1000 - (size_t)(v & 0xFFF), n = 0;
    while (n + 1 < cap && n < room) { unsigned char c = pb[n]; if (c < 0x20 || c >= 0x7f) break; buf[n++] = c; }
    if (n >= 5 && n < room && pb[n] == 0) { buf[n] = 0; return n; }
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
            if (!obj_page_mapped(base + off)) break;
            uint64_t v = *(const uint64_t *)(uintptr_t)(base + off);
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
    fprintf(stderr, "[dm] CRASH sig=%d", sig);
    off("addr", (uint64_t)si->si_addr); off("pc", OC_SS_PC(uc)); off("lr", OC_SS_LR(uc));
    // pc 处三条指令的机器码：用来核对 pc→模块偏移有没有错位（本例 addr=0 但按反汇编前一条也该炸，得确认取到的 pc 真是那条）
    { uintptr_t pp = (uintptr_t)OC_SS_PC(uc);
      if (module_of((void *)pp) && (pp & 0xFFF) >= 8) {
          volatile uint32_t *w = (volatile uint32_t *)pp;
          fprintf(stderr, " 指令=%08x %08x [%08x]", w[-2], w[-1], w[0]);
      } }
    fprintf(stderr, " sp=%#llx", OC_SS_SP(uc));
    { char td[200] = ""; if (g_thr_desc) g_thr_desc(td, sizeof td); fprintf(stderr, " 线程=%s", td); }   // 崩的是哪条客体线程（槽/名/线程函数）| which guest thread crashed
    fprintf(stderr, "\n");
    fprintf(stderr, " fp=%#llx\n", OC_SS_FP(uc));
    for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%#llx%s", i, OC_SS_X(uc,i), i % 6 == 5 ? "\n" : " ");
    fprintf(stderr, "\n");
    // 客体把「错误说明」存在对象字段里、交给日志函数打印（FMOD 事件系统就是这么存的）；崩在解引用上时没人来得及打，
    // 所以我们自己按字段翻一遍。崩在处理器里再崩一次会丢整份现场（g_in_crash 直接 _exit(9)），
    // 所以每个地址先用 mincore 确认页是映射的，并且只在同一页内取串。
    crash_dump_objects(uc);
    // 线程参数对象：空指针崩在客体线程上时，这里通常能直接看出"对象被释放/清零"（整块 0）还是"某个字段没设过"
    { extern uint64_t thr_cur_arg(void); uint64_t a = thr_cur_arg();
      fprintf(stderr, "[dm] 线程参数=%#llx\n", (unsigned long long)a);
      if (a && !(a & 7)) {
          for (int i = 0; i < 80; i++) {
              uint64_t v = 0; vm_size_t g2 = 0;
              if (vm_read_overwrite(mach_task_self(), a + (uint64_t)i * 8, 8, (vm_address_t)&v, &g2) || g2 != 8) {
                  fprintf(stderr, "[dm]   参数+%#x 读不到\n", i * 8); break;
              }
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
    dm_backtrace(OC_SS_FP(uc));
    { extern void hle_hist_dump(void); hle_hist_dump(); }
    { extern void hle_trace_dump(void); hle_trace_dump(); }   // 分线程的真调用序列（DM_HLTRACE；记名桩本来就在记）| per-thread real call sequence
    { extern void nvn_dump_counts(const char *); nvn_dump_counts("崩溃时"); }
    { extern void hlecount_dump_now(void); hlecount_dump_now(); }   // 崩溃时"哪些剪口真被调过、各几次"：0 条 = 客体根本没走到这些接口 | Which HLE entries actually ran at crash time: no lines = the guest never reached those interfaces   // 崩溃时 NVN 各类调用计数：0 = 根本没摸到图形层 | NVN call counts at crash time: 0 = the graphics layer was never touched
    crash_unwind();
    // 客栈窗扫描：帧链常断（客体把 x29 当普通寄存器用），从 sp 起在同页内拷 2 KB，
    // 落在客体模块区间里的值基本都是返回地址 —— 用来还原崩溃前的客体调用链。
    // Guest stack-window scan: the frame chain often breaks (guest code uses x29 as a scratch register); copy 2 KB from
    // sp within its own page; values falling inside guest modules are almost always return addresses.
    {
        uint64_t sp = OC_SS_SP(uc);
        if (sp > 0x10000 && !(sp & 7)) {
            uint64_t v[256]; size_t n = sizeof v, room = 4096 - (size_t)(sp & 0xFFF);
            if (room < n) n = room;
            memcpy(v, (const void *)(uintptr_t)sp, n);
            fprintf(stderr, "[dm] 客栈窗:");
            for (size_t k = 0; k < n / 8; k++) {
                module_t *mm = module_of((void *)(uintptr_t)v[k]);
                if (mm && v[k] >= (uint64_t)mm->base) fprintf(stderr, " %s+%#llx", mm->name, v[k] - (uint64_t)mm->base);
            }
            fputc('\n', stderr);
        }
    }
   // 最近的宿主→客体调用，便于定位“错值从哪一步交出去” | recent host->guest calls, to locate which step handed out a bad value
    // 诊断：从 sp 起 8 KB，凡落入客体模块或宿主 .so 区间的字都打出来（带符号），用来还原调用链
    {
        uint64_t sp = OC_SS_SP(uc); Dl_info di; uint64_t hb = 0, he = 0;
        if (dladdr((void *)&on_crash, &di) && di.dli_fbase) { hb = (uint64_t)di.dli_fbase; he = hb + 0x400000; }
        fprintf(stderr, "[dm] 宿text %#llx..%#llx\n", (unsigned long long)hb, (unsigned long long)he);
        if (sp > 0x10000 && !(sp & 7)) {
            size_t n = 8192;
            for (size_t k = 0; k < n / 8; k++) {
                uint64_t v = ((volatile uint64_t *)(uintptr_t)sp)[k];
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
    }

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
