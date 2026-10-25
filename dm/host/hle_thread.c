// Horizon 内核的线程与同步 SVC，原生实现：每个客体线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待挂在一把全局锁 G 上，每线程一个条件变量；先求正确，性能后量。
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#include "dm.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/syscall.h>
// SuspendThread 的真实停车：SG_SUSPEND 让目标线程进处理器、SG_RESUME 放行。
// 采样器占用 SIGUSR2，这里用 SIGRTMIN（musl 下 34 起，宿主无人用）。
// Real parking for SuspendThread: SG_SUSPEND enters the handler, SG_RESUME releases. SIGUSR2 belongs to the
// sampler; SIGRTMIN is unclaimed on musl.
#define SG_SUSPEND (SIGRTMIN)
#define SG_RESUME  (SIGRTMIN + 1)
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

// 诊断：已退出客体线程名单（thread_exit 只记录不打印——栈小；现场导出时补打）
// Diagnostic: roster of exited guest threads (thread_exit only records, never prints -- small stacks; printed later in the state dump)
static struct { uint32_t h; uint64_t nsvc, exit_ns; int last_svc; uint64_t last_lr; } g_grave[16]; static int g_ngrave;

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr;
    // 等待时的客体帧链（等待类 svc 入口一次捕齐，dump 用；OHOS 没有 Mach 线程挂起，thr_bt 打不出回溯）
    // Guest frame chain captured once at the entry of a waiting SVC (for the dump; OHOS lacks Mach thread
    // suspension so thr_bt cannot produce backtraces there)
    uint64_t bt[10]; int nbt;
    uint64_t wregs[4];   // 等待类 svc 入口的 x19..x22（0x3d56b0 的 x19=作业池）| x19..x22 at the waiting SVC's entry
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    mach_port_t mport;         // 宿主线程端口（取 CPU 时间；OHOS shim 恒 0） | Host thread port (to get CPU time; the OHOS shim returns 0)
    int tid;                   // Linux 线程号（gettid；读 /proc/self/task/<tid> 的 wchan/状态用——OHOS 无 Mach） | Linux thread id (for /proc wchan/state)
    // SuspendThread/ResumeThread(0x32/0x33，Horizon 专用)的真实现：
    // int suspend_count>0 且 suspended=1 = 被停；wake() 见 suspended 只记 wake_pending 不唤醒；
    // 在跑客体代码的线程用 tgkill(SG_SUSPEND) 停在信号处理里，Resume 用 SG_RESUME 放行。
    // Real implementation of SuspendThread/ResumeThread (0x32/0x33, Horizon-only). wake() defers to
    // wake_pending while suspended; a thread running guest code is stopped inside a signal handler.
    int suspend_count; volatile int suspended, susp_ack, wake_pending, in_svc, hook_parked; int64_t pend_res;
    uint32_t qos;
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        // 注意：不跳过 suspended 的等待者。真机内核对「被暂停的等待者」照常完成交接（握手、拿锁），
        // 线程停在「醒来但暂停」态，Resume 后带着锁继续跑。我们的等价实现：完整唤醒 → 块返回 →
        // svc 出口钩子停车（见 svc_thread 包装）。跳过反而把信号弄丢、清错键（v61/v62 实锤）。
        // Do NOT skip suspended waiters: the real kernel completes the handoff for paused waiters (they stop
        // "woken but paused" and continue with the lock on resume). Our equivalent: full wake -> block returns ->
        // the svc-exit hook parks them. Skipping loses the signal (measured in v61/v62).
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) {
    wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv);
}

// ---------- SuspendThread / ResumeThread（0x32/0x33）真实实现 ----------
// 真机：ThreadManagerImplByHorizon 的 Unsafe 变体直发内核（停/放目标 OS 线程）。
// 宿主： blocked 线程挂 suspended 标志（wake 让路）；svc 中的线程延迟到出口补停；其余停在 SG_SUSPEND 处理器。
// （sg_park / 处理器 / 安装在 svc_thread 包装前定义；见下方 svc_thread。）
// Real device: the Horizon ThreadManager Unsafe variants go straight to the kernel. Host: blocked threads get a
// suspended flag; mid-svc threads defer to the svc-exit hook; the rest park in the SG_SUSPEND handler.
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
static int64_t block(thr_t *t, int kind, int64_t ns) {   // 启动时间线：按等待种类累计（全线程）+ 主线程单列 | Boot timeline: accumulated by wait kind (all threads) + the main thread listed separately
    if (!g_flag.bootprof) return block_inner(t, kind, ns);
    uint64_t t0 = now_ns(); int64_t r = block_inner(t, kind, ns); uint64_t d = now_ns() - t0;
    static const int id[] = {0, PROF_WAIT_LOCK, PROF_WAIT_CV, PROF_WAIT_ADDR, PROF_WAIT_SYNC};
    dm_prof_add(id[kind & 7], d, 0);
    if (t->handle == 0x1001) dm_prof_add(PROF_MAIN_WAIT, d, 0);
    return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        // 被挂起（SuspendThread）时超时冻结：等待时钟不走、不脱队。否则限时会带着「挂起」的线程超时出队，
        // 它会在游戏以为世界已停的时候跑客体代码、错过发给它的信号（信号按无等待者清键），恢复后死等 —— v61 实锤。
        // While suspended, the wait is frozen: the timeout clock stops and the thread stays queued. Otherwise a
        // timed wait expires, the thread runs guest code while the game believes the world is stopped, and the
        // signal meant for it is consumed as "no waiters" -- deadlock on resume (measured in v61).
        if (t->suspended) { pthread_cond_wait(&t->cv, &G); continue; }
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = pthread_cond_wait(&t->cv, &G);
        } else rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取当前游戏拍；diag.c 初始化时登记（单测不登记，拍号记 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每线程 TSD 槽（门、x18、TLS、TLR）唯一设置点 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
void hle_trace_tag(uint32_t handle);   // hle.c：DM_HLTRACE 的分线程调用环选槽 | per-thread call ring slot (hle.c)
void crash_set_thr_desc(void (*fn)(char *, size_t));   // crash.c：把「我是哪条客体线程」接进崩溃现场 | hand the thread descriptor to the crash scene
void thr_cur_desc(char *out, size_t n);   // 定义在下面 | defined below
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    hle_trace_tag(t->handle);                            // DM_HLTRACE：本线程的调用环按句柄选槽 | pick this thread's call ring by handle
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self());
    t->tid = (int)syscall(SYS_gettid);
    crash_thread_init();
    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    dm_thread_enter(t->entry, t->arg, t->stack_top);   // 不返回：客体须经 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
static const char *thr_name(thr_t *t, char *buf, size_t n);
static void thread_exit(thr_t *t) {
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    t->exited = 1;
    if (g_ngrave < 16) { g_grave[g_ngrave].h = t->handle; g_grave[g_ngrave].nsvc = t->nsvc; g_grave[g_ngrave].exit_ns = t->exit_ns; g_grave[g_ngrave].last_svc = t->last_svc; g_grave[g_ngrave].last_lr = t->last_lr; g_ngrave++; }
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
void dm_thread_returned(void) { die("客体线程入口函数返回（Horizon 要求 ExitThread）"); }

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_key_create(&g_keythr, NULL);
    crash_set_thr_desc(thr_cur_desc);
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt); t->tid = (int)syscall(SYS_gettid);
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    hle_trace_tag(t->handle);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    return t->handle;
}

static uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客体主线程写；诊断行在同一线程读 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle == 0x1001) continue;
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    pthread_mutex_unlock(&G);
    if (count) *count = n;
    return qos;
}

// ---------- 同步对象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
static const char *thr_name(thr_t *t, char *nb, size_t n) {   // SDK ThreadType+0x1a0 = 名字指针 | SDK ThreadType+0x1a0 = name pointer
    if (!t->arg) return t->handle == 0x1001 ? "主线程" : "?";
    uint64_t np = 0; vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), t->arg + 0x1a0, 8, (vm_address_t)&np, &got) || !np ||
        vm_read_overwrite(mach_task_self(), np, n - 1, (vm_address_t)nb, &got)) return "?";
    nb[got] = 0; return nb;
}
// 全部活着的客体线程（采样器 DM_SAMPLE=毫秒:* 用）：把 pthread 填进 out，返回个数
// All live guest threads (used by the sampler DM_SAMPLE=ms:*): fill the pthreads into out, return the count
int thr_list(pthread_t *out, int max) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t && n < max; t = t->all) if (!t->exited && t->started) out[n++] = t->pt;
    pthread_mutex_unlock(&G);
    return n;
}
// 浸泡读数（DM_SOAK）：句柄表占用、存活客体线程、线程对象总数（含已退出未释放）
// Soak readings (DM_SOAK): handle table usage, live guest threads, total thread objects (including exited but not yet freed)
void thr_counts(int *handles, int *live, int *objs) {
    int nh = 0, nl = 0, no = 0;
    pthread_mutex_lock(&G);
    for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    for (thr_t *t = g_all; t; t = t->all) { no++; nl += t->started && !t->exited; }
    pthread_mutex_unlock(&G);
    *handles = nh; *live = nl; *objs = no;
}
// 按 pthread 取客体线程名（按线程 CPU 采样 DM_THRCPU 用，memstat.m）：不是客体线程返回 0
// Get the guest thread name by pthread (used by per-thread CPU sampling DM_THRCPU, memstat.m): returns 0 if not a guest thread
int thr_name_of(pthread_t pt, char *out, size_t n) {
    int ok = 0; char nb[64];
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (t->pt == pt && !t->exited) { snprintf(out, n, "%#x %s", t->handle, thr_name(t, nb, sizeof nb)); ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
uint32_t thr_cur_handle(void) { thr_t *t = pthread_getspecific(g_keythr); return t ? t->handle : 0; }   // 只读 TSD，信号处理里可用 | Only reads TSD, usable in a signal handler
// 心跳用：按客体句柄取线程名（"UnityGfxDeviceWorker" 这类）。拿不到锁就回 0，心跳不为一行名字挡住客体线程。
// Heartbeat: guest thread name by handle (names like "UnityGfxDeviceWorker"). Returns 0 when the lock is busy -- a heartbeat
// line is not worth blocking a guest thread for.
int thr_name_by_handle(uint32_t handle, char *out, size_t n) {
    if (pthread_mutex_trylock(&G)) return 0;
    int ok = 0; char nb[64];
    for (thr_t *t = g_all; t; t = t->all) if (t->handle == handle && t->started && !t->exited) { snprintf(out, n, "%s", thr_name(t, nb, sizeof nb)); ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
// 每线程最后一次 SVC 号（采样行打出来）：分辨「在等」和「在某个 SVC 里空转」。句柄 0x1001..0x10FF 取低字节当槽。
// Per-thread last SVC number (printed on the sample line) to tell "blocked" from "spinning inside an SVC".
// Handles 0x1001..0x10FF use their low byte as the slot.
_Atomic int g_last_svc[256];
// 同槽再存一份「那次 SVC 的 x0」：采样行能直接看出主线程睡多久、等的是哪个键（0x0B 的 ns / 0x1C、0x18 的地址）。
// Same slot also stores that SVC's x0: the sample line then shows how long the thread sleeps and which key it waits on
// (0x0B = ns, 0x1C/0x18 = address).
_Atomic uint64_t g_last_svc_a0[256];
unsigned thr_cur_slot(void) { thr_t *t = pthread_getspecific(g_keythr); return t ? (t->handle & 0xffu) : 0; }
// 崩溃现场用：本线程的客体线程参数（CreateThread 的 x2）。世界创建崩在 UpdateJobThread_0 的
// For the crash dump: this guest thread's CreateThread argument. World creation dies on AcquireSemaphore(NULL) in
// UpdateJobThread_0; the object behind that argument worked since boot and only went empty at world creation, so dump it
// as-is to tell a freed/zeroed object from a field that was never set.
uint64_t thr_cur_arg(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    return t ? t->arg : 0;
}
// 崩溃现场用：我是哪个客体线程（槽号 / SDK 给的名字 / 入口函数 / 参数）。不碰 G 锁，只读本线程 TSD + 客体内存，
// 所以在信号处理里安全（线程可能正是死在持锁的 SVC 里）。
// For the crash dump: which guest thread am I (slot, SDK thread name, entry function, argument). Takes no locks -- it only
// reads this thread's TSD and guest memory, so it is safe inside a signal handler (the thread may have died holding G).
void thr_cur_desc(char *out, size_t n) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) { snprintf(out, n, "非客体线程"); return; }
    char nb[64]; const char *nm = thr_name(t, nb, sizeof nb);
    // SDK 给的 entry 是蹦床；ThreadType+0x1d0 才是蹦床 blr 过去的线程函数本体（见上面 CreateThread 的诊断）。
    // 现场要的是这条线程自己跑哪个函数，所以取 0x1d0。
    // The entry the SDK passes is a trampoline; ThreadType+0x1d0 is the thread function the trampoline blr's to (see the
    // CreateThread diagnostic above). The crash scene wants the function this thread actually runs, so read 0x1d0.
    uint64_t real = 0, v1d0 = 0; vm_size_t g2 = 0;
    if (t->arg && !(t->arg & 7) &&
        !vm_read_overwrite(mach_task_self(), t->arg + 0x1d0, 8, (vm_address_t)&v1d0, &g2) && g2 == 8) real = v1d0;
    module_t *m = module_of((void *)(real ? real : t->entry));
    uint64_t e = real ? real : t->entry;
    snprintf(out, n, "槽 %#x「%s」入口=%s%s%#llx%s 参数=%#llx 优先级 %d 核 %d 已调 svc %llu",
             t->handle, nm, m ? m->name : "?", m ? "+" : "=",
             (unsigned long long)(m ? e - (uint64_t)m->base : e), real ? "（真）" : "",
             (unsigned long long)t->arg, t->prio, t->core, (unsigned long long)t->nsvc);
}
// 按线程名找活着的客体线程（采样器用）；找不到返回 0
// Find a live guest thread by thread name (used by the sampler); returns 0 if not found
int thr_find_by_name(const char *name, pthread_t *out) {
    char nb[32]; int ok = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited && !strcmp(thr_name(t, nb, sizeof nb), name)) { *out = t->pt; ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
// 现场导出（看门狗 / 崩溃时由 crash.c 调；只读、不取锁）：每个客体线程在等什么、最近一次 SVC 从哪发出
// 现场导出时取别的客体线程的回溯：暂停 → 读寄存器 → 沿帧指针链走（客体与门块都保持 x29 链）→ 恢复；只打 main / sdk 帧
// State dump (called from crash.c on watchdog / crash; read-only, takes no lock): what each guest thread is waiting on and where its most recent SVC was issued from
// During a state dump, take the backtrace of other guest threads: suspend → read registers → walk the frame pointer chain (both guest code and gate blocks keep the x29 chain) → resume; only main / sdk frames are printed
static void thr_bt(thr_t *t) {
    if (!t->mport || t->exited || t->mport == mach_thread_self()) return;
    if (thread_suspend(t->mport) != KERN_SUCCESS) return;
    arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
        // 没在等系统调用的线程（在跑客体代码，可能在空转等一个条件）：打被调用者保存寄存器 x19–x28，以及 x19 / x20 指向的内存各 128 字节。
        // 偶发「进局后不出帧」现场：主线程在作业池的「边帮忙边等」（main+0x14f08d8）里空转，x19 = 等待对象、x20 = 作业池——有这些才能看出它在等哪一批、未完成计数是几
        // Threads not waiting in a system call (running guest code, possibly spinning on a condition): print the callee-saved registers x19–x28, plus 128 bytes of the memory pointed to by each of x19 / x20.
        // The state seen in the occasional "no frames after entering a run": the main thread spins in the job pool's "help while waiting" (main+0x14f08d8), x19 = wait object, x20 = job pool -- only with these can one tell which batch it is waiting for and what the unfinished count is
        if (t->wkind == 0) { char rl[400]; int q = snprintf(rl, sizeof rl, "[dm]   %#x 寄存器 pc=", t->handle); uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc);
            q += pm ? snprintf(rl + q, sizeof rl - q, "%s+%#llx", pm->name, pc - (uint64_t)pm->base) : snprintf(rl + q, sizeof rl - q, "%#llx", pc);
            for (int i = 19; i <= 28; i++) q += snprintf(rl + q, sizeof rl - q, " x%d=%#llx", i, st.__x[i]);
            fprintf(stderr, "%s\n", rl);
            for (int i = 19; i <= 20; i++) { uint8_t raw[128]; vm_size_t g = 0; uint64_t a = st.__x[i];
                if (a < 0x100000000ull || vm_read_overwrite(mach_task_self(), a, sizeof raw, (vm_address_t)raw, &g) || g != sizeof raw) continue;
                char hx[420]; int h = 0; for (int k = 0; k < 128; k += 4) h += snprintf(hx + h, sizeof hx - h, "%08x ", *(uint32_t *)(raw + k));
                uint64_t vt = *(uint64_t *)raw; module_t *vm_ = module_of((void *)vt); char vs[64] = ""; if (vm_) snprintf(vs, sizeof vs, "（首 8 字节 = %s+%#llx）", vm_->name, vt - (uint64_t)vm_->base);
                fprintf(stderr, "[dm]   %#x x%d → %#llx 起 128 字节%s: %s\n", t->handle, i, a, vs, hx); } }
        uint64_t fp = __darwin_arm_thread_state64_get_fp(st), lr = __darwin_arm_thread_state64_get_lr(st);
        // 在跑客体代码的线程：栈顶往上 1 KiB 原样打出来（外层各帧保存的寄存器在里面，事后能找回等待对象、资源地址）
        // Threads running guest code: print the 1 KiB above the stack top as is (the registers saved by the outer frames are in there, so the wait object and resource address can be recovered afterwards)
        if (t->wkind == 0 && module_of((void *)__darwin_arm_thread_state64_get_pc(st))) { uint64_t sp = __darwin_arm_thread_state64_get_sp(st), stk[128]; vm_size_t g = 0;   // pc 在客体模块里才打：在睡眠 / 让出里的线程 pc 在系统库，栈没看头 | Printed only if pc is inside a guest module: a thread in sleep / yield has its pc in a system library, and the stack is not worth looking at
            if (sp > 0x100000000ull && !vm_read_overwrite(mach_task_self(), sp, sizeof stk, (vm_address_t)stk, &g) && g == sizeof stk)
                for (int r = 0; r < 128; r += 8) fprintf(stderr, "[dm]   %#x 栈 sp+%#05x: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n", t->handle, r * 8,
                        stk[r], stk[r + 1], stk[r + 2], stk[r + 3], stk[r + 4], stk[r + 5], stk[r + 6], stk[r + 7]); }
        char line[1024]; int p = snprintf(line, sizeof line, "[dm]   %#x 回溯:", t->handle), nf = 0; uint64_t wait_fp = 0;
        for (int k = 0; k < 64 && fp && nf < 20 && p < (int)sizeof line - 40; k++) {
            module_t *m = module_of((void *)lr);
            if (m) { p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, lr - (uint64_t)m->base); nf++; }
            uint64_t fr[2]; vm_size_t g2 = 0;
            if (vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
            if (m && !strcmp(m->name, "main") && lr - (uint64_t)m->base == 0x132db30) wait_fp = fp;   // 返回点在资源等待函数 main+0x132d988 里（它调作业池「边帮忙边等」之后）：此刻的帧指针就是它自己的 | The return point is inside the resource wait function main+0x132d988 (after it calls the job pool's "help while waiting"): the frame pointer at this moment is its own
            fp = fr[0]; lr = fr[1];
        }
        fprintf(stderr, "%s\n", line);
        // 资源等待函数 main+0x132d988（序言 sub sp,#0x50 … add x29,sp,#0x40）在自己栈底摆等待对象 {虚表, 指向事件指针的指针}（0x132db14 stp x8,x23,[sp]），
        // 所以等待对象 = 它的帧指针 −0x40；所指 −0x10 = 资源等待对象 {+0 在途作业, +8 互斥锁指针, +0x10 事件指针}。按返回点 0x132db30 定位，与线程此刻停在等待循环的哪一条指令无关。
        // 旧写法按 0x14f0934 取「上一帧」取错一层，把资源等待对象当成等待对象读，「在途作业 为空」「事件 / 锁 读不到」都是错读
        // The resource wait function main+0x132d988 (prologue sub sp,#0x50 … add x29,sp,#0x40) places the wait object {vtable, pointer to the event pointer} at the bottom of its own stack (0x132db14 stp x8,x23,[sp]),
        // so the wait object = its frame pointer −0x40; what that points to, −0x10 = the resource wait object {+0 in-flight job, +8 mutex pointer, +0x10 event pointer}. It is located by the return point 0x132db30, regardless of which instruction of the wait loop the thread is stopped on right now.
        // The old code took "the previous frame" based on 0x14f0934 and was off by one level, reading the resource wait object as the wait object; 「在途作业 为空」(in-flight job is null) and 「事件 / 锁 读不到」(event / lock unreadable) were both misreads
        if (wait_fp) { uint64_t wo[2] = {0, 0}, res[3] = {0, 0, 0}, wjob = 0; vm_size_t g = 0;
            #define RD(addr, buf) ((addr) > 0x100000000ull && !vm_read_overwrite(mach_task_self(), (addr), sizeof(buf), (vm_address_t)(buf), &g) && g == sizeof(buf))
            #define HEX(tag, addr, nbytes) { uint32_t raw[32]; vm_size_t gg = 0; if ((addr) > 0x100000000ull && !vm_read_overwrite(mach_task_self(), (addr), (nbytes), (vm_address_t)raw, &gg) && gg == (nbytes)) { \
                char hx[300]; int h = 0; for (unsigned q = 0; q < (nbytes) / 4; q++) h += snprintf(hx + h, sizeof hx - h, "%08x ", raw[q]); fprintf(stderr, "[dm]   %#x 作业池等待：%s %#llx: %s\n", t->handle, tag, (unsigned long long)(addr), hx); } \
                else fprintf(stderr, "[dm]   %#x 作业池等待：%s %#llx 读不到\n", t->handle, tag, (unsigned long long)(addr)); }
            if (RD(wait_fp - 0x40, wo)) { module_t *vm_ = module_of((void *)wo[0]);
                fprintf(stderr, "[dm]   %#x 作业池等待：等待对象 %#llx 虚表 %s+%#llx（应为 main+0x20f2408），所指 %#llx\n", t->handle, (unsigned long long)(wait_fp - 0x40), vm_ ? vm_->name : "?", vm_ ? wo[0] - (uint64_t)vm_->base : wo[0], wo[1]);
                if (RD(wo[1] - 0x10, res)) {
                    fprintf(stderr, "[dm]   %#x 作业池等待：资源等待对象 %#llx：在途作业 %#llx（%s）、互斥锁 %#llx、事件 %#llx。判别：在途作业为空 = 信号已被某次等待消费；非空且事件未置位 = 完成信号没来；事件已置位 = 等待循环的问题\n",
                            t->handle, (unsigned long long)(wo[1] - 0x10), res[0], res[0] ? "非空" : "为空", res[1], res[2]);
                    HEX("事件本体 64 字节", res[2], 64);
                    HEX("互斥锁本体 32 字节（+8 重入计数、+0x10 持有线程对象、+0x18 锁字 = 持有者句柄）", res[1], 32);
                    // 作业对象（提交函数 main+0x147f154 填的）：+0x10 标志（取作业时按线程类别掩码过滤，0xeda190 / 0xdf8fac）、+0x20 队列里的下一个、+0x30 起依赖表（低 31 位 = 个数）、+0x60 资源、+0x68 指向事件指针的指针
                    // Job object (filled by the submit function main+0x147f154): +0x10 flags (filtered by a per-thread-class mask when taking a job, 0xeda190 / 0xdf8fac), +0x20 next in the queue, dependency table from +0x30 (low 31 bits = count), +0x60 resource, +0x68 pointer to the event pointer
                    if (res[0]) { HEX("在途作业对象 +0 起 128 字节", res[0], 128); HEX("在途作业对象 +0x80 起 128 字节", res[0] + 0x80, 128); } wjob = res[0]; } }
            // 作业池：线程正停在等待循环 main+0x14f08d8 自己的指令上时 x20 就是它；停在它调出去的函数里时 x20 不可信，不打
            // Job pool: when the thread is stopped on an instruction of the wait loop main+0x14f08d8 itself, x20 is the pool; when it is stopped in a function called from it, x20 cannot be trusted and is not printed
            { uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc); uint64_t off = pm && !strcmp(pm->name, "main") ? pc - (uint64_t)pm->base : 0;
              // 队列（0xeda190）：{+0 尾, +8 头, +0x10 个数, +0x18 锁}，节点 +0x10 标志、+0x20 下一个。顺着头走最多 8 个，看被等的作业在不在这个池里
              // Queue (0xeda190): {+0 tail, +8 head, +0x10 count, +0x18 lock}, node +0x10 flags, +0x20 next. Walk from the head for at most 8 nodes to see whether the awaited job is in this pool
              if (off >= 0x14f0900 && off < 0x14f09b8) { uint64_t qa = st.__x[20] + 0x410, nd = 0; HEX("作业队列头（池 = x20，+0x410 起 32 字节）", qa, 32); int in = 0, cnt = 0;
                  if (RD(qa + 8, &nd)) for (; nd && cnt < 8; cnt++) { uint64_t nb[5] = {0}; if (!RD(nd, nb)) break; fprintf(stderr, "[dm]   %#x 作业池等待：  队列第 %d 个 %#llx 标志 %#x%s\n", t->handle, cnt, (unsigned long long)nd, (uint32_t)nb[2], nd == wjob ? "（就是被等的作业）" : ""); in |= nd == wjob; nd = nb[4]; }
                  fprintf(stderr, "[dm]   %#x 作业池等待：被等的作业%s x20 这个池的队列前 %d 个里\n", t->handle, in ? "在" : "不在", cnt); }
              else fprintf(stderr, "[dm]   %#x 作业池等待：线程不在等待循环自己的指令上，作业池没取\n", t->handle); }
            #undef RD
            #undef HEX
        }
    }
    thread_resume(t->mport);
}
void svc_thread_dump(void) {
    static const char *kinds[] = {"运行/未在等", "锁", "条件变量", "地址", "同步对象", "自挂起"};
    int nh = 0; for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    fprintf(stderr, "[dm] 句柄表占用 %d / %d，累计建线程 %llu\n", nh, MAX_H, g_next_id - 0x51);
    for (thr_t *t = g_all; t; t = t->all) {
        module_t *m = module_of((void *)t->last_lr);
        // 线程名：SDK ThreadType+0x1a0 是名字指针（ThreadManager::SetInitialThreadNameUnsafe 0x34a2a4）；CreateThread 的 x2 = ThreadType*
        // Thread name: SDK ThreadType+0x1a0 is the name pointer (ThreadManager::SetInitialThreadNameUnsafe 0x34a2a4); x2 of CreateThread = ThreadType*
        char nb[32]; const char *nm = thr_name(t, nb, sizeof nb);
        double cpu = -1;
        if (t->mport && !t->exited) { thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS)
                cpu = bi.user_time.seconds + bi.user_time.microseconds / 1e6 + bi.system_time.seconds + bi.system_time.microseconds / 1e6; }
        char own[96] = ""; if (t->wkind == 1 && t->waddr) { uint32_t w = 0; vm_size_t g2 = 0;   // 锁等待：锁字 = 持有者句柄 | 0x40000000 | Lock wait: lock word = owner handle | 0x40000000
            if (!vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2)) snprintf(own, sizeof own, " 持有者 %#x", w & ~WAITERS_BIT); }
        if (t->wkind == 2 && t->waddr) { uint32_t w = 0, k = 0; vm_size_t g2 = 0;              // 条件等待：关联锁字与条件键的当前值（卡死现场缺这一项） | Condition wait: current values of the associated lock word and the condition key (the deadlock dump lacked this item)
            vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2); vm_read_overwrite(mach_task_self(), t->wkey, 4, (vm_address_t)&k, &g2);
            snprintf(own, sizeof own, " 锁字 %#x 条件键 %#llx=%#x", w, t->wkey, k); }
        uint8_t raw[64]; vm_size_t g3 = 0;                                                   // 所等对象（读写锁 / 条件变量所在结构）原始 64 字节：看计数与持有者 | Raw 64 bytes of the object being waited on (the structure holding the read-write lock / condition variable): to see the counts and the owner
        int have_raw = (t->wkind == 1 || t->wkind == 2 || t->wkind == 3) && t->waddr && !vm_read_overwrite(mach_task_self(), t->waddr & ~0xfull, 64, (vm_address_t)raw, &g3) && g3 == 64;
        char life[96]; uint64_t nowns = now_ns();
        if (t->exited) snprintf(life, sizeof life, " 建于拍 %u、退于拍 %u（%.1f 秒前）", t->born_tick, t->exit_tick, (nowns - t->exit_ns) / 1e9); else if (!t->born_ns) snprintf(life, sizeof life, " 进程启动时就在"); else snprintf(life, sizeof life, " 建于拍 %u（%.1f 秒前）", t->born_tick, (nowns - t->born_ns) / 1e9);
        fprintf(stderr, "[dm] 线程 %#x 「%s」优先级 %d 核 %d 掩码 %#llx CPU %.2fs svc %llu %s%s%s 等:%s addr=%#llx%s 最近 svc %#x lr=%s+%#llx\n", t->handle, nm, t->prio, t->core, (unsigned long long)t->mask,
                cpu, t->nsvc, t->exited ? "已退出" : "", t->started ? "" : "未启动", life, kinds[t->wkind & 7], t->waddr, own, t->last_svc,
                m ? m->name : "?", m ? t->last_lr - (uint64_t)m->base : t->last_lr);
        if (have_raw) { char hx[200]; int q = 0; for (int i = 0; i < 64; i += 4) q += snprintf(hx + q, sizeof hx - q, "%08x ", *(uint32_t *)(raw + i));
                        fprintf(stderr, "[dm]   %#x 所等对象 %#llx 起 64 字节: %s\n", t->handle, t->waddr & ~0xfull, hx); }
        // 等待时帧链：svc 入口捕的（见 svc_thread）。主线程 50ms 轮询那种不进等待 svc 的，这里没有。
        // Frame chain captured at the waiting SVC's entry. Threads that poll without entering a wait SVC (the
        // main thread's 50 ms spin) have no chain here.
        if (t->nbt > 0) { char bl[600]; int q = 0;
            for (int k = 0; k < t->nbt && q < (int)sizeof bl - 48; k++) { module_t *bm = module_of((void *)t->bt[k]);
                q += bm ? snprintf(bl + q, sizeof bl - q, " %s+%#llx", bm->name, t->bt[k] - (uint64_t)bm->base)
                        : snprintf(bl + q, sizeof bl - q, " %#llx", t->bt[k]); }
            fprintf(stderr, "[dm]   %#x 等待帧链:%s\n", t->handle, bl); }
        if (t->nbt > 0)
            fprintf(stderr, "[dm]   %#x 等待现场 x19=%#llx x20=%#llx x21=%#llx x22=%#llx\n", t->handle,
                    (unsigned long long)t->wregs[0], (unsigned long long)t->wregs[1],
                    (unsigned long long)t->wregs[2], (unsigned long long)t->wregs[3]);
        // 内核侧真相：/proc 状态位 + wchan。R=在跑（可能客体内自旋）、S/D=睡、wchan=futex=宿主锁/condvar 里
        // Kernel-side truth from /proc: state char + wchan. R = running (may spin in guest code), S/D = sleeping.
        if (t->tid > 0) {
            char pp[64]; char st = '?'; long wcv = 0;
            snprintf(pp, sizeof pp, "/proc/self/task/%d/stat", t->tid);
            int fd = open(pp, O_RDONLY);
            if (fd >= 0) { char sb[512]; ssize_t r = read(fd, sb, sizeof sb - 1); close(fd);
                if (r > 0) { sb[r] = 0; char *rp = strrchr(sb, ')');
                    if (rp && rp[1] == ' ' && rp[2]) st = rp[2]; } }
            snprintf(pp, sizeof pp, "/proc/self/task/%d/wchan", t->tid);
            fd = open(pp, O_RDONLY);
            if (fd >= 0) { char wb[64]; ssize_t r = read(fd, wb, sizeof wb - 1); close(fd);
                if (r > 0) { wb[r] = 0; wcv = strtol(wb, NULL, 16); } }
            fprintf(stderr, "[dm]   %#x 内核态=%c wchan=%#lx\n", t->handle, st, wcv);
        }
        thr_bt(t);
    }
    for (int gi = 0; gi < g_ngrave; gi++) {   // 诊断：已退名单（含死因：最后 svc 号与 lr） | Diagnostic: exited roster (with cause of death: last svc number and lr)
        module_t *gm = module_of((void *)g_grave[gi].last_lr);
        fprintf(stderr, "[dm] 已退线程 %#x svc %llu（%.2f 秒前退）最后 svc %#x lr=%s+%#llx\n", g_grave[gi].h, (unsigned long long)g_grave[gi].nsvc, (now_ns() - g_grave[gi].exit_ns) / 1e9,
                g_grave[gi].last_svc, gm ? gm->name : "?", gm ? (unsigned long long)(g_grave[gi].last_lr - (uint64_t)gm->base) : (unsigned long long)g_grave[gi].last_lr);
    }
    for (thr_t *t2 = g_all; t2; t2 = t2->all) if (t2->waddr) {
        uint32_t v = 0; vm_size_t g2 = 0;
        if (!vm_read_overwrite(mach_task_self(), t2->waddr & ~3ull, 4, (vm_address_t)&v, &g2))
            fprintf(stderr, "[dm] 线程 %#x 上次等待地址 %#llx 现值 %#x（等类型 %d）\n", t2->handle, (unsigned long long)t2->waddr, v, t2->wkind);
    }
    // 宿主自建线程（预取、Granny 后台、图形出图、音频等）：不在客体线程表里，按 mach 线程枚举，dladdr 符号化前 12 帧
    // Threads created by the host itself (prefetch, Granny background, graphics present, audio, etc.): not in the guest thread table, enumerated as mach threads, the first 12 frames symbolized with dladdr
    thread_act_array_t ths; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &ths, &nth) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < nth; i++) {
            int guest = 0; for (thr_t *t = g_all; t; t = t->all) if (t->mport == ths[i]) { guest = 1; break; }
            if (!guest && ths[i] != mach_thread_self() && thread_suspend(ths[i]) == KERN_SUCCESS) {
                arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(ths[i], ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
                    char line[1200]; int p = snprintf(line, sizeof line, "[dm] 宿主线程 %#x:", ths[i]);
                    uint64_t fp = __darwin_arm_thread_state64_get_fp(st), pc = __darwin_arm_thread_state64_get_pc(st);
                    for (int k = 0; k < 12 && p < (int)sizeof line - 80; k++) {
                        Dl_info di; module_t *m = module_of((void *)pc);
                        if (m) p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, pc - (uint64_t)m->base);
                        else if (dladdr((void *)pc, &di) && di.dli_sname) p += snprintf(line + p, sizeof line - p, " %s+%#lx", di.dli_sname, (unsigned long)(pc - (uint64_t)di.dli_saddr));
                        else p += snprintf(line + p, sizeof line - p, " %#llx", pc);
                        uint64_t fr[2]; vm_size_t g2 = 0;
                        if (!fp || vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
                        fp = fr[0]; pc = fr[1];
                    }
                    fprintf(stderr, "%s\n", line);
                }
                thread_resume(ths[i]);
            }
            mach_port_deallocate(mach_task_self(), ths[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ths, nth * sizeof *ths);
    }
}

// DM_THRDUMP=<毫秒>：定时把全部客体线程在等什么打一遍（世界启动这类长等待现场用；不设=不开）
// DM_THRDUMP=<ms>: periodically dump what every guest thread is waiting on (for long waits such as world start; unset = off)
// 每第 4 个周期把「读侧安全」的取证打进去（NVN 计数 / NVN trace 末段 / 剪口计数）：
// 「线程活着但图形链路停摆」这类现场等不来崩溃，取证就得定时送。
// 注意 hle_hist_dump / hle_trace_dump 不能在这里调：它们的环条目被客体并发改写，计时线程读到
// 毒值名字指针就是一次 SIGSEGV（v35 前的 22769 号 crash 实锤）；崩溃处理器在信号上下文里调它们才安全。
// Every 4th tick emits the read-safe forensics (NVN counts / NVN trace tail / HLE counts): a "threads alive but the
// graphics pipeline stalled" scene never crashes, so the evidence has to be delivered on a timer. hle_hist_dump /
// hle_trace_dump must NOT be called here: their ring entries are mutated by the guest concurrently, and the timer
// thread reads poisoned name pointers (crash 22769 proved it); only the signal-context crash handler may walk those.
static void *thr_dump_loop(void *arg) {
    long ms = (long)(intptr_t)arg;
    extern void nvn_dump_counts(const char *);
    extern void nvn_trace_dump(const char *, uint64_t);
    extern void hlecount_dump_now(void);
    // DM_SELFCRASH=<秒>：到点在计时线程上 raise(SIGSEGV)，借崩溃处理器把全套取证（调用环/近迹/NVN 计数）
    // 落进 crash_<pid>.txt——「不崩但卡死」的现场用它换一份崩溃级证据。崩溃处理器在信号上下文里走环是安全的
    // （v35 的 22769 号 crash 验证过这条路径）；从计时线程直接走环则会读到客体并发改写的毒值指针（同案反例）。
    // DM_SELFCRASH=<sec>: raise(SIGSEGV) on the timer thread at the deadline so the crash handler writes a full
    // forensic crash_<pid>.txt for a "stalled, not crashed" scene. Walking the rings from signal context is safe
    // (crash 22769 proved this path); walking them from a plain timer thread reads poisoned pointers (same case).
    static int64_t self_crash_s = -2;
    if (self_crash_s == -2) { const char *e = getenv("DM_SELFCRASH"); self_crash_s = e && *e ? atoll(e) : 0; }
    // DM_FORCEE0=<秒>：诊断开关。到点把 main 的 JThread 计数 [G+0xe0] 强制清零（主线程在 main+0x18e9450
    // 50ms 轮询它，卡在 1 不掉）。只动这一个字，看游戏是否继续——用来证明「唯一卡点」，
    // 不是修复；结论出来后按正当方式处理。
    // DM_FORCEE0=<sec>: diagnostic. At the deadline force the JThread pending count [G+0xe0] to zero (the main
    // thread polls it at main+0x18e9450; it is stuck at 1). One word only -- proves whether this is the sole
    // blocker. Not a fix.
    static int64_t force_e0_s = -2;
    if (force_e0_s == -2) { const char *e = getenv("DM_FORCEE0"); force_e0_s = e && *e ? atoll(e) : 0; }
    int e0_forced = 0;
    // DM_FORCESEQ=<秒>：诊断。调度器自旋环（main+0x197e5f0）等 [S3→[8] 头节点 +0x18] 序号前进；
    // 到点把该序号 +1（比 FORCEE0 安全：不动环结构，只模拟"有人推进了一格"），看 0x1024 是否出旋、游戏是否前进。
    // DM_FORCESEQ=<sec>: diagnostic. The scheduler spin waits for the head node's +0x18 serial to advance;
    // bump it by one at the deadline (safer than FORCEE0: no ring-structure edits) and watch whether the runner unblocks.
    static int64_t force_seq_s = -2;
    if (force_seq_s == -2) { const char *e = getenv("DM_FORCESEQ"); force_seq_s = e && *e ? atoll(e) : 0; }
    int seq_forced = 0;
    int64_t t0_s = -1;
    for (unsigned tick = 0;; tick++) {
        int64_t now_s = (int64_t)(time(NULL) & 0x7fffffff);
        if (t0_s < 0) t0_s = now_s;
        if (self_crash_s > 0 && now_s - t0_s >= self_crash_s) {
            fprintf(stderr, "[dm] DM_SELFCRASH=%lld 到点，自取崩溃现场\n", (long long)self_crash_s);
            fflush(stderr);
            raise(SIGSEGV);
        }
        usleep((useconds_t)(ms * 1000));
        svc_thread_dump();
        if (tick % 4) continue;
        fprintf(stderr, "[dm] ===== 定时取证 tick %u =====\n", tick);
        // 定点探针：HSS 主线程在 main+0x18e9450 的等待环里轮询 P=*main[0x293a918]、G=*P，
        // 等 [G+0xe0]==0 且 [G->b8]+8 字节 ==0。IsDone 门（main+0x15ff240）另查 main[0x293f268]→P2→G2
        // 的 [G2+0xe0] 与 [S2+0x14]。两条链一起读，卡哪条一眼可见。客体堆就是宿主内存，空闲期读不会 fault。
        // Targeted probe: the main thread polls P=*main[0x293a918], G=*P for [G+0xe0]==0 and [G->b8]+8==0;
        // the IsDone gate (main+0x15ff240) also checks main[0x293f268]→P2→G2 for [G2+0xe0] and [S2+0x14].
        {
            uint8_t *mb = NULL;
            for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { mb = g_mod[i].base; break; }
            for (int gi2 = 0; gi2 < 3 && mb; gi2++) {
            uint64_t slot = gi2 == 0 ? 0x293a918 : gi2 == 1 ? 0x293f268 : 0x2940228;   // G3=调度器链（0x197e900 用）
            char tag = "123"[gi2];
            volatile uint64_t *pp = (volatile uint64_t *)(mb + slot);
            uint64_t P = *pp, G = P > 0x10000 ? *(volatile uint64_t *)P : 0;
            static uint64_t s_P[3], s_G[3], s_e0[3]; static int s_said;
            if (s_said <= gi2) s_said = gi2 + 1;
            if ((P != s_P[gi2] || G != s_G[gi2]) && s_said > gi2 + 1 - 1 && s_P[gi2])
                fprintf(stderr, "[dm] G探针%c 变化 P %#llx→%#llx G %#llx→%#llx\n", tag, (unsigned long long)s_P[gi2], (unsigned long long)P, (unsigned long long)s_G[gi2], (unsigned long long)G);
            s_P[gi2] = P; s_G[gi2] = G;
            if (G > 0x10000) {
                uint64_t S = *(volatile uint64_t *)(G + 0xb8);
                uint32_t e0 = *(volatile uint32_t *)(G + 0xe0);
                if (e0 != s_e0[gi2]) { fprintf(stderr, "[dm] G探针%c pending %u→%u @tick %u\n", tag, s_e0[gi2], e0, tick); s_e0[gi2] = e0; }
                fprintf(stderr, "[dm] G探针%c G=%#llx e0=%u d8=%u dc=%u S=%#llx S+8=%u S+0x14=%u\n",
                        tag, (unsigned long long)G, e0,
                        *(volatile uint32_t *)(G + 0xd8), *(volatile uint32_t *)(G + 0xdc),
                        (unsigned long long)S, S > 0x10000 ? (unsigned)*(volatile uint8_t *)(S + 8) : 0,
                        S > 0x10000 ? (unsigned)*(volatile uint8_t *)(S + 0x14) : 0);
                if (gi2 == 2) {   // G3=调度器链：自旋环等头节点序号（main+0x197e5f0 读 [head+0x18]）
                    uint64_t head = S > 0x10000 ? *(volatile uint64_t *)(S + 8) : 0;
                    if (head > 0x10000) {
                        uint32_t seq = *(volatile uint32_t *)(head + 0x18);
                        fprintf(stderr, "[dm] G3头节点=%#llx 序号=%u 窗=%016llx %016llx %016llx %016llx\n",
                                (unsigned long long)head, seq,
                                (unsigned long long)*(volatile uint64_t *)head,
                                (unsigned long long)*(volatile uint64_t *)(head + 8),
                                (unsigned long long)*(volatile uint64_t *)(head + 0x10),
                                (unsigned long long)*(volatile uint64_t *)(head + 0x20));
                        if (force_seq_s > 0 && !seq_forced && now_s - t0_s >= force_seq_s) {
                            *(volatile uint32_t *)(head + 0x18) = seq + 1;
                            seq_forced = 1;
                            fprintf(stderr, "[dm] DM_FORCESEQ 到点：头节点序号 %u→%u\n", seq, seq + 1);
                        }
                    } else fprintf(stderr, "[dm] G3头节点空（S=%#llx）\n", (unsigned long long)S);
                }
                // 作业池字段监视（每 4 拍，仅 gi2==0，放在 G 可达判断之外）：主线程泵循环(0x3d5bd0)的池在
                // 0x606dc4b9xx 邻域；dump 0x606dc4b900..0x606dc4bb00（覆盖 0x1023 的 sem 0x606dc4b954 与
                // 主线程 sem 0x606dc4ba38）。池头认法：+0x230 计数非零或 +0x380 队列数非零。
                // Pool-field watch (outside the G-reachable check): dump the window covering both known semaphores.
                if (gi2 == 0) {
                    volatile uint64_t *wlo = (volatile uint64_t *)0x606dc4b6e0ull;
                    char px[1200]; int pq = 0;
                    for (int qw = 0; qw < 140 && pq < 1150; qw++) {
                        uint64_t v = wlo[qw];
                        pq += snprintf(px + pq, sizeof px - pq, "%llx:%llx ", (unsigned long long)(0x606dc4b6e0ull + qw * 8), (unsigned long long)v);
                    }
                    fprintf(stderr, "[dm] 池窗 %s\n", px);
                }
                // 全结构 dump(认线程句柄 0x10xx / pthread id / 环项数组)| full struct dump (spot thread handles / ring array)
                { char hx[900]; int q = 0;
                  for (int o = 0x20; o <= 0xb0 && q < 800; o += 8)
                      q += snprintf(hx + q, sizeof hx - q, "%llx ", (unsigned long long)*(volatile uint64_t *)(G + o));
                  fprintf(stderr, "[dm] G探针%c G+20..b0: %s\n", tag, hx); }
                // G+0xa8 是栈上指针：落到哪个客体线程的栈区，就说明环主是它
                // G+0xa8 lands on some guest thread's stack: that thread owns the ring.
                { uint64_t sa = *(volatile uint64_t *)(G + 0xa8);
                  if (sa > 0x100000000ull) { char nb2[32]; int matched = 0;
                  for (thr_t *t = g_all; t; t = t->all) {
                      if (sa <= t->stack_top && sa > t->stack_top - 0x200000) {
                          fprintf(stderr, "[dm] G探针%c G+a8=%#llx → 线程 %#x「%s」栈顶 %#llx\n", tag, (unsigned long long)sa, t->handle, thr_name(t, nb2, sizeof nb2), (unsigned long long)t->stack_top);
                          matched = 1; } }
                  if (!matched) fprintf(stderr, "[dm] G探针%c G+a8=%#llx 不属于任何存活线程的栈（环主已退或从未启动）\n", tag, (unsigned long long)sa); } }
                // 存活线程的栈范围全列一遍（配对 G+a8 / 环项栈指针用）
                { char nb3[32];
                  for (thr_t *t = g_all; t; t = t->all)
                      fprintf(stderr, "[dm] 栈表 %#x「%s」%#llx-%#llx%s\n", t->handle, thr_name(t, nb3, sizeof nb3), (unsigned long long)(t->stack_top - 0x200000), (unsigned long long)t->stack_top, t->started ? "" : " 未启动"); }
                // 环项数组候选 G+0x58 / G+0x80 / G+0x90 / G+0x98 的目标各 dump 64 字节
                { static const int cands[] = { 0x58, 0x80, 0x90, 0x98 };
                  for (unsigned ci = 0; ci < sizeof cands / sizeof cands[0]; ci++) {
                      uint64_t a = *(volatile uint64_t *)(G + cands[ci]);
                      if (a <= 0x100000000ull) continue;
                      char hx2[300]; int q2 = 0;
                      for (int o = 0; o < 64 && q2 < 280; o += 8) q2 += snprintf(hx2 + q2, sizeof hx2 - q2, "%llx ", (unsigned long long)*(volatile uint64_t *)(a + o));
                      fprintf(stderr, "[dm] G探针%c G+%#x→%#llx: %s\n", tag, cands[ci], (unsigned long long)a, hx2);
                      // 项 0 的"代码指针"（数组首 8 字节）是哪个模块的：解出积压项要调什么
                      { module_t *im = module_of((void *)*(volatile uint64_t *)a);
                        fprintf(stderr, "[dm] G探针%c 项0.code=%#llx → %s+%#llx（前16字节 %016llx %016llx）\n", tag,
                                (unsigned long long)*(volatile uint64_t *)a,
                                im ? im->name : "?", im ? *(volatile uint64_t *)a - (uint64_t)im->base : 0,
                                (unsigned long long)*(volatile uint64_t *)(*(volatile uint64_t *)a),
                                (unsigned long long)*(volatile uint64_t *)(*(volatile uint64_t *)a + 8)); } } }
                if (force_e0_s > 0 && !e0_forced && now_s - t0_s >= force_e0_s) {
                    // 两条链一起"消费"：读下标推进到写下标（=消费掉积压项），待办清零。只作诊断：
                    // 证明积压环是唯一卡点后，再回头找正当的消费路径。
                    // "Consume" both rings at once: advance the read index to the write index (= drain the
                    // backlog) and zero the pending count. Diagnostic only.
                    uint32_t d8 = *(volatile uint32_t *)(G + 0xd8), dc = *(volatile uint32_t *)(G + 0xdc);
                    *(volatile uint32_t *)(G + 0xd8) = dc;
                    *(volatile uint32_t *)(G + 0xe0) = 0;
                    fprintf(stderr, "[dm] DM_FORCEE0 到点：G%u 已消费环 d8 %u→%u、e0→0\n", gi2 + 1, d8, dc);
                    if (gi2 == 1) e0_forced = 1;
                }
            } else fprintf(stderr, "[dm] G探针%c P=%#llx 不可达\n", tag, (unsigned long long)P);
            }
        }
        nvn_dump_counts("定时");
        nvn_trace_dump("定时", 0);
        hlecount_dump_now();
        fflush(stderr);
    }
    return NULL;
}
// DM_SPUR=<毫秒>：实验开关。定期给条件等待一次伪唤醒（内核允许；客体的 while 循环会重查条件）。
// 用来判定「世界启动互等」是不是丢唤醒：真丢了信号的话，这一下就能让等的一方重新检查并继续。
// DM_SPUR=<ms>: experiment. Periodically issue a spurious wakeup to condition waiters (allowed by the kernel; the guest's
// while loop re-checks). Used to tell whether the world-start deadlock is a lost wakeup: if a signal was dropped, this lets
// the waiting side re-check and move on.
static void *spur_loop(void *arg) {
    long ms = (long)(intptr_t)arg;
    for (;;) {
        usleep((useconds_t)(ms * 1000));
        pthread_mutex_lock(&G);
        for (thr_t *w = g_wait; w; ) { thr_t *n = w->next;
            if (w->wkind == 2) lock_acquire_for(w, w->waddr, w->wtag);   // 必须像 0x1D 一样把锁一并交给它：Wait 返回后客体按「持锁」继续
            w = n; }
        pthread_mutex_unlock(&G);
    }
    return NULL;
}
static _Atomic int g_spur_state;
void spur_maybe_start(void) {
    if (atomic_load_explicit(&g_spur_state, memory_order_relaxed)) return;
    int expect = 0;
    if (!atomic_compare_exchange_strong(&g_spur_state, &expect, 1)) return;
    const char *e = getenv("DM_SPUR");
    if (!e || !*e || atol(e) <= 0) return;
    pthread_t th;
    if (!pthread_create(&th, NULL, spur_loop, (void *)(intptr_t)atol(e))) pthread_detach(th);
    LOG("DM_SPUR=%s：条件等待每 %s 毫秒伪唤醒一次", e, e);
}

// 惰性启动：构造期可能还没拿到 --ps 传进来的环境；首次 SVC 时再判断。
// Lazily started: constructors may run before --ps env vars are set, so decide on the first SVC.
// 同时把 stderr 改道到 $DM_CRASHDIR/thr.txt：dump 行很长，走 hilog 会被吞。
// Also redirect stderr to $DM_CRASHDIR/thr.txt: the dump lines are long and hilog drops them.
static _Atomic int g_td_state;
// sp 反查线程（nvn_trace_dump 用）：栈区归属；无主 = 线程已退或栈已归还
// Map an sp to its owning guest thread (for nvn_trace_dump): no owner = the thread exited / stack returned.
const char *thr_at_sp(uint64_t sp) {
    static _Thread_local char buf[80];
    if (sp <= 0x100000000ull) return "sp无效";
    for (thr_t *t = g_all; t; t = t->all)
        if (t->stack_top && sp <= t->stack_top && sp > t->stack_top - 0x200000) {
            char nb[32];
            snprintf(buf, sizeof buf, "%#x「%s」", t->handle, thr_name(t, nb, sizeof nb));
            return buf;
        }
    snprintf(buf, sizeof buf, "无主(已退)");
    return buf;
}

void thr_dump_maybe_start(void) {
    if (atomic_load_explicit(&g_td_state, memory_order_relaxed)) return;
    int expect = 0;
    if (!atomic_compare_exchange_strong(&g_td_state, &expect, 1)) return;
    const char *e = getenv("DM_THRDUMP");
    if (!e || !*e || atol(e) <= 0) return;
    const char *d = getenv("DM_CRASHDIR");
    if (!d || !*d) d = getenv("DM_SAVE");
    if (d && *d) {
        char p[512]; snprintf(p, sizeof p, "%s/thr.txt", d);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 2); close(fd); }
    }
    pthread_t th;
    if (!pthread_create(&th, NULL, thr_dump_loop, (void *)(intptr_t)atol(e))) pthread_detach(th);
}

// svc 中补停 + 处理器共用：设 ack、等 SG_RESUME。handler 与 svc 出口钩子都走这里。
// Park helper shared by the signal handler and the svc-exit hook.
static void sg_park(thr_t *me) {
    me->susp_ack = 1;
    sigset_t wait; sigemptyset(&wait); sigaddset(&wait, SG_RESUME);
    while (me->suspended) sigsuspend(&wait);   // Resume: tgkill(SG_RESUME) 打断 | released by the resume signal
}
static void sg_suspend_handler(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; (void)uc_;
    thr_t *me = pthread_getspecific(g_keythr);
    if (!me) return;
    me->susp_ack = 1;
    if (me->suspended) sg_park(me);
}
static void sg_resume_handler(int sig, siginfo_t *si, void *uc_) { (void)sig; (void)si; (void)uc_; }   // sigsuspend 返回即放行 | sigsuspend returns = released
static int sg_installed;
static void sg_install(void) {
    if (sg_installed) return; sg_installed = 1;
    struct sigaction sa = {0};
    sa.sa_sigaction = sg_suspend_handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SG_SUSPEND, &sa, 0);
    sa.sa_sigaction = sg_resume_handler;
    sigaction(SG_RESUME, &sa, 0);
}
static int tgkill(int pid, int tid, int sig) { return syscall(SYS_tgkill, pid, tid, sig); }

// svc 包装：进去置 in_svc、出来若挂着 suspended 就地补停——绝不在「svc 中途/持 G」时用信号停车
// （v55 教训：把持 G 或持客体锁的线程停进信号处理器，全场冻结 → PreloadManager 看门狗 Break）。
// svc wrapper: park deferred to svc exit, never mid-svc while locks are held (the v55 lesson).
static int svc_thread_inner(int num, uint64_t *regs);
int svc_thread(int num, uint64_t *regs) {
    thr_t *me = pthread_getspecific(g_keythr);
    if (me) me->in_svc = 1;
    int r = svc_thread_inner(num, regs);
    if (me) {
        me->in_svc = 0;
        if (me->suspended && me->suspend_count > 0) { me->hook_parked = 1; sg_park(me); me->hook_parked = 0; }   // 补停：就地等 Resume | deferred park
    }
    return r;
}

static int svc_thread_inner(int num, uint64_t *regs) {
    { thr_t *me = pthread_getspecific(g_keythr); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->nsvc++;
      // 等待族 svc：捕获客体帧链（x29 链：[fp]=下一帧 [fp+8]=返回地址）。客体栈就是宿主内存，只读不写；
      // 帧 8 字节对齐、单调递增、步幅 < 8MiB，最多 10 帧——坏链就地截断。
      // Waiting-family SVCs: capture the guest frame chain (x29 chain). The guest stack is host memory,
      // read-only here; frames must be aligned, monotonic and < 8 MiB apart, up to 10 frames.
      if (num == 0x1a || num == 0x1b || num == 0x1c || num == 0x1d) {
          extern int dm_rd16(uint64_t, uint64_t[2]);   // hle.c：process_vm_readv 读 16 字节，EFAULT 不崩 | reads via process_vm_readv; EFAULT instead of a fault
          uint64_t fp = regs[29]; int nb = 0;
          me->bt[nb++] = regs[30];
          while (fp > 0x100000000ull && nb < 10) {
              uint64_t fr[2];
              if (!dm_rd16(fp, fr) || fr[0] <= fp || fr[0] - fp > (8ull << 20)) break;
              me->bt[nb++] = fr[1];
              fp = fr[0];
          }
          me->nbt = nb;
          // 等待帧链第 1 帧的调用者保存寄存器窗（x19..x22）：0x3d56b0 的 x19=作业池——
          // PreloadManager 卡死时这里就是「它隶属的池」指针，配合池猎手定位。
          // Callee-saved window of the first frame: x19 of 0x3d56b0 IS the job pool pointer.
          for (int ri = 0; ri < 4; ri++) me->wregs[ri] = regs[19 + ri];
      } } }
    if (g_flag.thr && num != 0x0B) { thr_t *me = pthread_getspecific(g_keythr); LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, regs[0], regs[1], regs[2], regs[3]); }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        pthread_cond_init(&t->cv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        if (g_flag.thr || g_flag.svc) { LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle, t->entry, t->arg, t->stack_top, t->prio, t->core);
            // 诊断：蹦床按 [TT+0x50]+[TT+0x58]-0x20 间接取线程真入口（见 sdk+0x350240 反汇编）；原始值 + 组合结果都打
            // Diagnostic: the trampoline fetches the thread's real entry indirectly as [TT+0x50]+[TT+0x58]-0x20 (see the disassembly of sdk+0x350240); print raw values and the combination
            if (t->arg && !(t->arg & 7)) {
                uint64_t v50 = 0, v58 = 0, v1d0 = 0, v1d8 = 0; vm_size_t g2 = 0;
                int ok = !vm_read_overwrite(mach_task_self(), t->arg + 0x50, 8, (vm_address_t)&v50, &g2) && g2 == 8
                       && !vm_read_overwrite(mach_task_self(), t->arg + 0x58, 8, (vm_address_t)&v58, &g2) && g2 == 8
                       && !vm_read_overwrite(mach_task_self(), t->arg + 0x1d0, 8, (vm_address_t)&v1d0, &g2) && g2 == 8
                       && !vm_read_overwrite(mach_task_self(), t->arg + 0x1d8, 8, (vm_address_t)&v1d8, &g2) && g2 == 8;
                if (ok) {
                    uint64_t comb = v50 + v58 - 0x20;
                    module_t *m = module_of((void *)comb);
                    module_t *m1 = v1d0 ? module_of((void *)v1d0) : 0;   // TT+0x1d0 = 蹦床 blr 的真委托（线程函数本体） | TT+0x1d0 = the real delegate the trampoline blr's to (the thread function itself)
                    LOG("  TT 0x50=%#llx 0x58=%#llx 组合(栈顶-0x20)=%#llx%s%s+%#llx | 1d0=%#llx%s%s+%#llx 1d8=%#llx", (unsigned long long)v50, (unsigned long long)v58, (unsigned long long)comb,
                        m ? " → " : "", m ? m->name : "", m ? (unsigned long long)(comb - (uint64_t)m->base) : 0,
                        (unsigned long long)v1d0, m1 ? " → " : "", m1 ? m1->name : "", m1 ? (unsigned long long)(v1d0 - (uint64_t)m1->base) : 0, (unsigned long long)v1d8);
                } else LOG("  TT 读取失败（arg=%#llx）", (unsigned long long)t->arg);
            } }
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];
        uint64_t t0 = g_flag.bootprof ? now_ns() : 0;
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else {   // 让出（0 / -1 / -2）。主线程的让出次数与耗时记账：游戏渲染任务图「等全部做完」是纯轮询 + 让出（main+0x15ee898），量它占一帧多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = pthread_getspecific(g_keythr);
            if (me && me->handle == 0x1001) { uint64_t y0 = now_ns(); sched_yield(); g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
            else sched_yield();
        }
        if (g_flag.bootprof) { uint64_t d = now_ns() - t0; dm_prof_add(PROF_SLEEP, d, 0); thr_t *me = pthread_getspecific(g_keythr); if (me && me->handle == 0x1001) dm_prof_add(PROF_MAIN_WAIT, d, 0); }
        return 1;
    }
    case 0x0C: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; return 1; }
    case 0x0D: { thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE; return 1; }
    case 0x0E: { thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2]; }
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: regs[0] = (uint64_t)(uint32_t)cur()->core; return 1;
    case 0x25: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        // 轮询风暴快照：主线程在 0x1C 上空转（世界启动卡死）时，把整条等待链原样打出来——谁在等哪个键/地址、
        // 是锁等待(1)还是条件等待(2)，一次就能看出是谁该唤醒谁而没唤。限流：每 20 万次一轮。
        // Poll-storm snapshot: while the main thread spins in 0x1C (world start hangs), dump the whole wait chain as is —
        // who waits on which key/address, lock (1) or condition (2) — which shows who should have woken whom. One round per 200k calls.
        { static _Atomic unsigned long g_n1c; unsigned long n1 = atomic_fetch_add(&g_n1c, 1);
          if ((n1 % 200000u) == 0) {
              LOG("0x1C 轮询 #%lu：thr %#x 地址=%#llx 键=%#llx 超时=%lld", n1, t->handle, (unsigned long long)addr,
                  (unsigned long long)key, (long long)regs[3]);
              for (thr_t *w = g_wait; w; w = w->next)
                  LOG("   等：thr %#x 种类=%d 键=%#llx 地址=%#llx", w->handle, w->wkind, (unsigned long long)w->wkey, (unsigned long long)w->waddr);
          }
          // 前 400 次条件等待的键值：和 0x1D 的信号键逐条对（世界启动握手互等时用）
          // Keys of the first 400 condition waits, to line up against the 0x1D signal keys (used when the world-start handshake deadlocks)
          if (n1 < 400) LOG("等待 key=%#llx addr=%#llx 超时=%lld thr=%#x", (unsigned long long)key, (unsigned long long)addr, (long long)regs[3], t->handle); }
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        // 有被挂起线程时不清键：信号可能是发给「停在信号处理器里的那个线程」的——它恢复后用户态快路径
        // 会看到非零键并消费；这时清键就把信号弄丢，游戏在停世界协议里死等（v66 实锤：Suspend(main) 后
        // 信号到 → 主线程已停在处理器里 → 键被清 → Resume 后主线程重睡，协议卡死）。
        // While any thread is suspended, do NOT clear the key: the signal may belong to a thread parked in the
        // suspend handler -- its user-mode fast path consumes the non-zero key after resume. Clearing it loses
        // the signal and deadlocks the stop-the-world protocol (measured in v66).
        int any_susp = 0; for (thr_t *w = g_all; w; w = w->all) if (w->suspended) { any_susp = 1; break; }
        if (!left && !any_susp) st32(key, 0);
        { static _Atomic int ns; unsigned k = atomic_fetch_add(&ns, 1);
          if (k < 400) LOG("信号 key=%#llx 个数=%d 唤醒=%d 余等待=%d", (unsigned long long)key, cnt, n, left); }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x32: {  // SuspendThread(w0 线程句柄)：真停。Horizon 语义=暂停计数，自挂起则睡到 Resume | SuspendThread: real stop. Horizon = pause count; self-suspend parks until Resume
        sg_install();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        pthread_mutex_lock(&G);
        int64_t r;
        if (!t) {   // 无效句柄=注册表里没填过的槽（v59：stop-the-world 第三个成员的句柄是调用者栈上垃圾）。按「无事可做」返回成功：
                    // 真句柄走真语义，垃圾句柄回到占位行为——否则游戏把这个失败当致命（Result=0xe401 Break）。
                    // Invalid handle = an unfilled registry slot. Treat as no-op so the stop-the-world proceeds.
            { static _Atomic int ns; unsigned k = atomic_fetch_add(&ns, 1);
              if (k < 20) LOG("SuspendThread %#x → 无效句柄，按成功返回", (unsigned)(uint32_t)regs[0]); }
            pthread_mutex_unlock(&G); regs[0] = 0; return 1;
        }
        else if (t->exited) { pthread_mutex_unlock(&G); regs[0] = 0; return 1; }   // 已退：无事可做 | exited: no-op
        else if (t == cur()) {                       // 自挂起：睡在 kind 5，Resume 唤醒 | self: park on kind 5
            t->suspend_count++;
            if (t->suspend_count == 1) { t->suspended = 1; block(t, 5, -1); }
            r = 0;
        } else if (t->wkind != 0 || t->in_svc) {     // 已在等待机器或 svc 中：记标志延迟（svc 出口补停 / block 冻结）| parked or mid-svc: defer
            t->suspend_count++;
            t->suspended = 1;
            r = 0;
        } else {                                     // 在跑客体代码：懒停车——只置标志，线程走到下一个 block()/svc 出口自停。
                                                     // 并等它真正停稳（进 block 或挂在出口钩子）再返回：游戏的下一步是给它发
                                                     // condvar 信号，若它还在乱跑信号就落在无等待者窗口被清（v66-v68 实锤）。
                                                     // Running guest code: lazy park, then WAIT (bounded) until the target has
                                                     // actually parked (entered block() or the exit hook) -- the game signals
                                                     // its condvar right after, and that signal must find it waiting.
            t->suspend_count++;
            t->suspended = 1;
            for (int i = 0; i < 4000 && t->wkind == 0 && !t->hook_parked && !t->exited; i++) {
                pthread_mutex_unlock(&G); usleep(500); pthread_mutex_lock(&G);
            }
            r = 0;
        }
        { static _Atomic int ns; unsigned k = atomic_fetch_add(&ns, 1);
          if (k < 40) { module_t *cm = module_of((void *)regs[30]);
              LOG("SuspendThread %#x → 槽 %#x 计数=%d%s 调用者=%s+%#llx", (unsigned)(uint32_t)regs[0],
                  t ? t->handle : 0, t ? t->suspend_count : 0, t == cur() ? "（自）" : "",
                  cm ? cm->name : "?", cm ? (uint64_t)regs[30] - (uint64_t)cm->base : regs[30]); } }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x33: {  // ResumeThread：句柄在 x1（x0=ThreadType*，sdk+0x130ebc 包装实测：x0=0x60b51e13d0 x1=0x1001/0x1005）；x0 兜底
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[1], O_THREAD);
        if (!t) t = h_get((uint32_t)regs[0], O_THREAD);
        if (!t || t->exited) {   // 都无效才走恢复全部兜底
            { static _Atomic int ns; unsigned k = atomic_fetch_add(&ns, 1);
              if (k < 20) { module_t *cm = module_of((void *)regs[30]);
                  LOG("ResumeThread x0=%#llx x1=%#llx → 都无效（调用者 %s+%#llx）：恢复全部被挂起线程", (unsigned long long)regs[0], (unsigned long long)regs[1],
                      cm ? cm->name : "?", cm ? (uint64_t)regs[30] - (uint64_t)cm->base : regs[30]); } }
            int nw = 0;
            for (thr_t *w = g_all; w; w = w->all) {
                if (!w->suspended || w == cur()) continue;
                w->suspended = 0; nw++;
                if (w->wkind == 5) wake(w, 0);
                else if (w->wkind == 0 && w->tid > 0) { static int pidr; if (!pidr) pidr = getpid(); tgkill(pidr, w->tid, SG_RESUME); }
                // kind 1..4 冻在 block() 里的不动：它们的交接已由信号侧完整完成 | kinds 1..4 frozen in block() stay; their handoffs were completed by the signal side
            }
            LOG("ResumeThread 无效句柄：已恢复 %d 条被挂起线程", nw);
            pthread_mutex_unlock(&G); regs[0] = 0; return 1;
        }
        if (t->suspend_count > 0) {
            t->suspend_count--;
            if (t->suspend_count == 0 && t->suspended) {
                t->suspended = 0;
                if (t->wkind == 5) wake(t, 0);            // 自挂起(kind 5)：补唤醒 | self-suspend: deliver the wake
                else if (t->wkind == 0 && t->tid > 0) {   // 信号停的 / svc 出口补停的：放行 | signal or exit-hook parked: release
                    static int pid2; if (!pid2) pid2 = getpid();
                    tgkill(pid2, t->tid, SG_RESUME);
                }
                // kind 1..4：还冻在 block() 里的（超时已冻结）原地继续等；等到的交接已按真机语义完成 | kinds 1..4 still frozen in block() keep waiting; completed handoffs already woke them
            }
        }
        { static _Atomic int ns; unsigned k = atomic_fetch_add(&ns, 1);
          if (k < 40) LOG("ResumeThread x0=%#llx x1=%#llx → 槽 %#x 计数=%d", (unsigned long long)regs[0], (unsigned long long)regs[1], t ? t->handle : 0, t ? t->suspend_count : 0); }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    }
    return 0;
}

// ---------- 服务层的 SystemEvent ----------
// 真机：服务把内核事件的读端交给客户端，客体经 SDK nn::os 等待。宿主：在客体线程上调 SDK 自己的
// CreateSystemEvent(跨进程=true)，SDK 经 SVC 0x45 建出内核事件；记下写端句柄，之后任何线程（含宿主音频线程）都可触发。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!pthread_getspecific(g_keythr)) die("hle_bind_system_event 须在客体线程上调用");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(void *ev) {   // 未绑定的按手动清除即时绑定（只能在客体线程上发生） | An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) { hle_bind_system_event(ev, 0); pthread_mutex_lock(&g_se_lock); wh = se_find(ev); pthread_mutex_unlock(&g_se_lock); }
    pthread_mutex_lock(&G);
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
    e->signaled = 1; wake_sync_waiters();
    pthread_mutex_unlock(&G);
}

__attribute__((used)) static const char hss_stack_mark[] = "hss-stack64m-v1";
