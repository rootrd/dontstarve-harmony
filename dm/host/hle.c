// 导入解析（已实现接宿主、其余接记名桩）与宿主门分派。
// Import resolution (implemented ones go to the host, the rest go to named stubs) and host gate dispatch.
#include "dm.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <dlfcn.h>
#include <time.h>
#include <stdatomic.h>   // _ZTH 给址那块的每线程表用原子量（见 tls_wrapper_block） | the per-thread table behind the _ZTH block needs atomics
#ifdef __linux__
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
// 等待帧链的取帧读：process_vm_readv 读本进程 16 字节，EFAULT 返回错误而不是 SIGSEGV——
// 帧链是客体栈上的数据，坏指针必须按「链断」处理，不能把游戏带走。
// Frame fetch for the wait-time backtrace: process_vm_readv on our own process returns EFAULT
// instead of raising -- a bad frame pointer means "chain broken", not a game crash.
int dm_rd16(uint64_t addr, uint64_t out[2]) {
    static int pid;
    if (!pid) pid = getpid();
    struct iovec l = { out, 16 }, r = { (void *)(uintptr_t)addr, 16 };
    return syscall(SYS_process_vm_readv, pid, &l, 1UL, &r, 1UL, 0UL) == 16;
}
#else
#include <string.h>
int dm_rd16(uint64_t addr, uint64_t out[2]) {
    if (addr < 0x100000000ull) return 0;
    memcpy(out, (const void *)(uintptr_t)addr, 16);
    return 1;
}
#endif

typedef struct { const char *name; void *fn; } hle_ent;

// ---------- 未实现导入：记名桩（entry.S 里静态汇编的桩表，第 k 个 = movz x9,#k; b unimpl_entry）----------
// ---------- Unimplemented imports: named stubs (the statically assembled stub table in entry.S, the k-th = movz x9,#k; b unimpl_entry) ----------
#define MAX_STUB 4096                 // 与 entry.S 的 .rept 次数一致 | Matches the .rept count in entry.S
static unsigned g_nstub;
static const char *g_stub_name[MAX_STUB];
extern uint64_t unimpl_stubs[MAX_STUB];   // 每桩 8 字节 | 8 bytes per stub

// 同一个「桩名 + 调用点」只打一行。饥荒实测两代教训：
//   ① 早先不去重，_ZTHN16cApplicationBase… 一族一轮 11759 行，把真正的现场（崩溃 bt、SVC 记录）挤出 hilog 环形缓冲；
//   ② B28 改成按桩下标去重仍然刷屏（同名 25333 行、lr 一模一样）—— 同一个符号在每个引用点各占一个桩下标，
//      下标变、名字与调用点不变，键里带上名字才是真的去重。
// One line per (stub NAME, call site). Two generations of lessons on Don't Starve:
//   (1) un-deduplicated, the _ZTHN16cApplicationBase... family produced 11759 lines in one round and pushed the real
//       evidence (crash backtraces, SVC records) out of hilog's ring buffer;
//   (2) B28 deduplicated by stub index and still flooded (25333 lines of one name, identical lr) -- one symbol gets a
//       distinct stub index per reference site, so the index varies while name and call site do not. The key needs the name.
#define UNI_SEEN 8192
#define UNI_LOG_MAX 256      // 一轮最多打这么多「桩名+调用点」的新行，再多也只计数：刷屏会把崩溃现场从 hilog 环形缓冲挤掉 | at most this many new (name, call-site) lines per round; beyond that only counting -- flooding pushes the crash evidence out of hilog's ring buffer
static uint64_t g_uni_seen[UNI_SEEN];
static unsigned g_uni_sup, g_uni_logged;

// ---------- _ZTH<var>：TLS 变量初始化包装 ----------
// 编译器把「取一个线程局部变量」编成调 _ZTH<var>，契约是返回 x0 = 这个线程里那个变量的地址。
// 饥荒实测 7 个这类符号在所有模块都没导出（cApplicationBase 的线程成员：sDontStarveSettings、mActiveGameIndex、
// mActiveGameInstance、mActiveGameLibConfig、mDedicatedServerMode、mLogName、mGetURL —— Nintendo 的 __tls_init 在 rtld 里）。
// 落成返回 0 的桩 = 客体拿着空对象去取成员 = 一进「读设置 / 活动游戏」那条路就闪退。
// 这里按 (符号, 客体线程) 给一块清零的存储：语义正好是「每线程一份、初值全零」，也保住了成员偏移。
// _ZTH<var>: the compiler turns "read a thread-local variable" into a call to it, and the contract is x0 = that variable's
// address in this thread. Seven of these are undefined in every module on Don't Starve (cApplicationBase's thread members
// sDontStarveSettings / mActiveGameIndex / mActiveGameInstance / mActiveGameLibConfig / mDedicatedServerMode / mLogName /
// mGetURL -- Nintendo's __tls_init lives in rtld). A stub returning 0 hands the guest a null object to read members from.
// Handing out one zeroed block per (symbol, guest thread) is exactly "one per thread, zero-initialised" and keeps the member offsets.
#define TLSW_MAX   64
#define TLSW_TH    64
#define TLSW_BYTES 4096
static struct { const char *name; void *blk[TLSW_TH]; } g_tlsw[TLSW_MAX];
static _Atomic unsigned g_tlsw_n;
static _Atomic unsigned g_tlsw_next;
static pthread_mutex_t g_tlsw_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned g_tlsw_slot = TLSW_TH;   // 越界 = 本线程还没领号（第一次调用时领） | out of range = this thread has no number yet (it takes one on its first call)

static void *tls_wrapper_block(const char *name) {
    if (g_tlsw_slot >= TLSW_TH) g_tlsw_slot = atomic_fetch_add(&g_tlsw_next, 1) % TLSW_TH;
    unsigned slot = g_tlsw_slot;
    for (unsigned i = 0, n = atomic_load_explicit(&g_tlsw_n, memory_order_acquire); i < n; i++) {
        if (g_tlsw[i].name != name) continue;     // 名字指针就是模块 dynstr 里那一份，按指针比即可 | the name pointer is the module's one dynstr copy, so compare pointers
        void *b = __atomic_load_n(&g_tlsw[i].blk[slot], __ATOMIC_ACQUIRE);
        if (!b) {
            pthread_mutex_lock(&g_tlsw_lock);
            if (!(b = g_tlsw[i].blk[slot]) && (b = calloc(1, TLSW_BYTES))) __atomic_store_n(&g_tlsw[i].blk[slot], b, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&g_tlsw_lock);
        }
        return b;
    }
    pthread_mutex_lock(&g_tlsw_lock);
    unsigned n = atomic_load(&g_tlsw_n);
    if (n < TLSW_MAX) {
        g_tlsw[n].name = name; memset(g_tlsw[n].blk, 0, sizeof g_tlsw[n].blk);
        atomic_store_explicit(&g_tlsw_n, n + 1, memory_order_release);
        LOG("_ZTH 桩改成给址：%s（每线程 %d 字节清零块）", name, TLSW_BYTES);
    }
    pthread_mutex_unlock(&g_tlsw_lock);
    return n < TLSW_MAX ? tls_wrapper_block(name) : NULL;
}

uint64_t unimpl_c(unsigned idx, uint64_t *regs) {
    extern void hle_hist_call(const char *, uint64_t, uint64_t, uint64_t);   // 定义在下面（DM_HLTRACE 的分线程调用环）| defined below
    hle_hist_call(g_stub_name[idx], regs[0], regs[1], regs[2]);   // 记名桩也进本线程调用环（不受 DM_HLTRACE 影响，环本来就为它而设）| named stubs also enter the per-thread call ring
    if (!strncmp(g_stub_name[idx], "_ZTH", 4)) {                  // TLS 包装的契约是「x0 = 变量地址」，给 0 就是给客体一个空对象 | its contract is "x0 = the variable's address"; returning 0 hands the guest a null object
        void *b = tls_wrapper_block(g_stub_name[idx]);
        if (b) return (uint64_t)(uintptr_t)b;
    }
    // 记下调用者（客体返回地址）：entry.S 把 x30 存在 regs 前一个槽。
    // 「某符号在所有模块都未定义」这类桩（如饥荒的 _ZTHN16cApplicationBase… TLS 初始化包装）
    // 光知道被调没用，要看是谁在调、按哪个 TLS 偏移取对象 —— 这个地址就是入口。
    // Record the caller (the guest return address): entry.S puts x30 in the slot before regs.
    // For stubs whose symbol is undefined in every module (e.g. Don't Starve's _ZTHN16cApplicationBase...
    // TLS-init wrappers) knowing the call happened is not enough: we need who calls it and which TLS offset
    // it uses for the object -- this address is the entry point for that.
    uint64_t lr = ((const uint64_t *)regs)[-1];
    module_t *cm = module_of((void *)lr);
    {   uint64_t fp = (lr >> 4) * 0x100000001B3ull;
        for (const char *q = g_stub_name[idx]; *q; q++) fp = (fp ^ (uint8_t)*q) * 0x100000001B3ull;   // 键里带桩名 | the key carries the stub name
        unsigned s = (unsigned)(fp >> 51) & (UNI_SEEN - 1);
        int hit = 0;
        for (int t = 0; t < 8; t++) { unsigned j = (s + t) & (UNI_SEEN - 1);
            if (g_uni_seen[j] == fp) { hit = 1; break; }
            if (!g_uni_seen[j]) { g_uni_seen[j] = fp; break; } }
        if (!hit) {
            if (g_uni_logged++ < UNI_LOG_MAX)
                LOG("UNIMPL %s 调用者=%s+%#llx (lr=%#llx)（x0=%#llx x1=%#llx x2=%#llx）", g_stub_name[idx],
                    cm ? cm->name : "?", (unsigned long long)(cm ? lr - (uint64_t)cm->base : lr),
                    (unsigned long long)lr, regs[0], regs[1], regs[2]);
            else g_uni_sup++;
        } else if (!(++g_uni_sup & 0x3ff))
            LOG("UNIMPL 抑制重复 %u 行（同桩名同调用点各只打一次）", g_uni_sup);
    }
    return 0;
}

void hle_hist(const char *name, uint64_t a, uint64_t b, uint64_t c);   // 定义在下面；崩溃现场打印最近调用 | defined below; the crash dump prints the recent calls

// 每模块一张符号号 → 已解析地址缓存
// Per module, one cache of symbol number → resolved address
static uint64_t g_cache[MAX_MOD][8192];

uint64_t resolve_import(module_t *m, const char *name) {
    // 链接器符号：客体的 PIE/自做重定位运行时靠它们找自己的重定位表；按本模块边界给地址（原先落成返回 0 的桩，
    // 客体就从空地址开始遍历重定位表 —— 饥荒实测撞这里）。
    // Linker symbols: the guest's PIE / self-relocating runtime uses these to find its own relocation tables; give this
    // module's bounds (they used to become stubs returning 0, so the guest walked relocation tables from a null address).
    if (!strcmp(name, "__rel_dyn_start")) return (uint64_t)(m->base + m->rela_off);
    if (!strcmp(name, "__rel_dyn_end"))   return (uint64_t)(m->base + m->rela_off + m->rela_sz);
    if (!strcmp(name, "__rel_plt_start")) return (uint64_t)(m->base + m->jmprel_off);
    if (!strcmp(name, "__rel_plt_end"))   return (uint64_t)(m->base + m->jmprel_off + m->plt_sz);
    static const hle_ent *sec; static unsigned long secsz;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &secsz);
    for (unsigned i = 0; i < secsz / sizeof(hle_ent); i++)
        if (!strcmp(sec[i].name, name)) {
            hle_hist(name, 0, 0, 0);
            if (g_flag.imports) { Dl_info di; dladdr(sec[i].fn, &di); LOG("import %s -> %s", name, di.dli_sname); }
            void *hlecount_wrap(const char *, const char *, void *); return (uint64_t)hlecount_wrap("HLE", name, sec[i].fn);   // DM_HLECOUNT 时为计数桩（含 libc 直通表） | With DM_HLECOUNT it is a counting stub (including the libc passthrough table)
        }
    // 已为同名分过桩就复用
    // Reuse the stub if one was already allocated for the same name
    void *hlecount_wrap(const char *, const char *, void *);
    for (unsigned k = 0; k < g_nstub; k++)
        if (!strcmp(g_stub_name[k], name)) return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
    if (g_nstub >= MAX_STUB) die("stub overflow");
    unsigned k = g_nstub++;
    g_stub_name[k] = name;
    (void)m;
    return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
}

#define HIST_N 32
typedef struct { const char *name; uint64_t a, b, c; } hist_ent;
static hist_ent g_hist[HIST_N]; static volatile unsigned g_hist_i;   // 诊断用，不追求原子 | diagnostics only, no atomics needed
// 记一条“宿主把值交给客体”的调用（门/导入/桩）；崩溃现场会连同回溯一起打出来
// Record one host->guest call (gate / import / stub); the crash dump prints these next to the backtrace
void hle_hist(const char *name, uint64_t a, uint64_t b, uint64_t c) {
    unsigned i = (g_hist_i++) % HIST_N;
    g_hist[i].name = name; g_hist[i].a = a; g_hist[i].b = b; g_hist[i].c = c;
}
void hle_hist_dump(void) {
    unsigned n = g_hist_i, start = n > HIST_N ? n - HIST_N : 0;
    for (unsigned k = start; k < n; k++) { const hist_ent *e = &g_hist[k % HIST_N];
        if (e->name) fprintf(stderr, "[dm] 近迹 %u: %s x0=%#llx x1=%#llx x2=%#llx\n", k, e->name, (unsigned long long)e->a, (unsigned long long)e->b, (unsigned long long)e->c); }
}

// ---------- DM_HLTRACE：每次「客体→宿主」调用记一条（名字 + x0..x2），按客体线程分成若干环 ----------
// 上面那条 32 格全局环记的是「导入解析」事件（一次性的），开机跑完就只剩解析顺序，崩溃现场看着像调用序列其实不是；
// 这条才是真调用序列，且按线程分开，能回答「崩的那条线程最后十几次宿主交互是什么、参数是什么」。
// 计数桩换形状见 hle_count.c（DM_HLTRACE=1 时生效，同时照旧累加 HLC 计数）。
// ---------- DM_HLTRACE: record every guest->host call (name + x0..x2), one ring per guest thread ----------
// The 32-slot global ring above records import-*resolution* events (one-shot), so once boot finishes it holds nothing but
// resolution order -- it looks like a call sequence in the crash dump but is not. This ring holds real calls and is split by
// thread, which is what answers "what were the last host interactions of the thread that crashed, with which arguments".
#define TR_RINGS 24
#define TR_N 32
#define TR_MAIN (0x1001u % TR_RINGS)      // 客体主线程句柄固定 0x1001 | the guest main thread handle is always 0x1001
typedef struct { const char *name; uint64_t a, b, c; } tr_ent;
static tr_ent g_tr[TR_RINGS][TR_N]; static unsigned g_tr_i[TR_RINGS];
static uint32_t g_tr_h[TR_RINGS];          // 这一槽当初登记时的客体线程句柄（心跳按句柄认线程）
static __thread unsigned g_tr_slot = TR_RINGS;   // 线程起跑时登记；非客体线程越界 => 不记 | registered at thread start; out of range = not a guest thread
void hle_trace_tag(uint32_t handle) { g_tr_slot = handle % TR_RINGS; g_tr_h[g_tr_slot] = handle; }
void hle_hist_call(const char *name, uint64_t a, uint64_t b, uint64_t c) {
    if (g_tr_slot >= TR_RINGS) return;
    unsigned i = (g_tr_i[g_tr_slot]++) % TR_N;
    g_tr[g_tr_slot][i] = (tr_ent){ name, a, b, c };
}
static void tr_print(unsigned slot, const char *tag) {
    unsigned n = g_tr_i[slot], start = n > TR_N ? n - TR_N : 0;
    fprintf(stderr, "[dm] %s：", tag);
    if (!n) { fprintf(stderr, "无记录\n"); return; }
    fprintf(stderr, "最近 %u 条（旧→新，序号是全局累计）\n", n - start);
    for (unsigned k = start; k < n; k++) { const tr_ent *e = &g_tr[slot][k % TR_N];
        fprintf(stderr, "[dm]   %u %s x0=%#llx x1=%#llx x2=%#llx\n", k, e->name,
                (unsigned long long)e->a, (unsigned long long)e->b, (unsigned long long)e->c); }
}
void hle_trace_dump(void) {   // 崩溃现场用：本线程一份、主线程一份（不同线程时才打第二份） | crash dump: this thread, plus the main thread when it is another one
    tr_print(g_tr_slot, "本线程调用");
    if (g_tr_slot != TR_MAIN) tr_print(TR_MAIN, "主线程调用");
}
// 心跳用：按客体线程句柄转储那一条调用环。环只有 24 个槽（句柄取模），线程多于 24 时后建的会盖掉同槽的旧线程，
// 心跳只在关心的几个句柄上转储，够用。
// Heartbeat: dump one call ring by guest thread handle. There are only 24 rings (handle modulo), so with more than 24
// threads a late one overwrites a same-slot thread's ring; the heartbeat only dumps the handful we care about.
void hle_trace_ring(uint32_t handle) {
    char tag[40]; snprintf(tag, sizeof tag, "调用环 thr=%#x", handle);
    tr_print(handle % TR_RINGS, tag);
}
// 心跳用（主推这条）：先一条环一行概览（句柄 / 累计条数 / 距上次心跳新增几条 / 最后三次调用的名字），
// 再把「这轮有新动作」的那几条环整条打出来（最多 3 条，加主线程）。
// 概览回答「谁在动、动的是不是图形层」；整条回答「它卡在哪一次调用之后」——客体不提交画面时，
// 图形线程的环尾就是它停下的那一步。
// Heartbeat (the main form of it): one line per non-empty ring (handle / total entries / new since the last beat / the last
// three call names), then the whole rings that moved this beat (at most 3, plus the main thread).
// The overview answers "who is running, and is it the graphics layer"; the full ring answers "which call did it stop after" --
// when the guest never submits a frame, the tail of the graphics thread's ring is the step it halted on.
void hle_trace_beat(void) {
    static unsigned prev[TR_RINGS];
    unsigned grow[TR_RINGS]; int ngrow = 0;
    extern int thr_name_by_handle(uint32_t, char *, size_t);
    fprintf(stderr, "[dm] 调用环概览（thr=客体句柄，末 3 = 最近三次宿主交互）\n");
    for (unsigned s = 0; s < TR_RINGS; s++) {
        unsigned n = g_tr_i[s], d = n - prev[s]; prev[s] = n;
        if (!n) continue;
        char nb[48] = ""; thr_name_by_handle(g_tr_h[s], nb, sizeof nb);
        fprintf(stderr, "[dm]   thr=%#x %s 计%u 新增%u 末3:", g_tr_h[s], nb, n, d);
        for (int k = 3; k >= 1; k--) {
            unsigned i = (n - 1 - (unsigned)k) % TR_N;
            if (n <= (unsigned)k) { fprintf(stderr, " -"); continue; }
            fprintf(stderr, " %s", g_tr[s][i].name ? g_tr[s][i].name : "?");
        }
        unsigned i = (n - 1) % TR_N;
        fprintf(stderr, "（末条 x0=%#llx）\n", (unsigned long long)g_tr[s][i].a);
        if (d && ngrow < TR_RINGS) grow[ngrow++] = s;
    }
    int shown = 0;
    for (int k = 0; k < ngrow && shown < 3; k++) {
        unsigned s = grow[k];
        if (s == TR_MAIN) continue;                 // 主线程单独必打 | the main thread is always dumped on its own
        if (g_tr_h[s] == 0) continue;
        fprintf(stderr, "[dm] —— 整条环 thr=%#x ——\n", g_tr_h[s]); tr_print(s, "环"); shown++;
    }
    fprintf(stderr, "[dm] —— 整条环 thr=0x1001（主线程）——\n"); tr_print(TR_MAIN, "环");
}

void stubs_report(void) { LOG("stubs=%u", g_nstub);
    for (unsigned i = 0; i < g_nstub; i++) LOG("  未实现桩: %s", g_stub_name[i]); }

// ---------- 宿主门分派 ----------
// 改写器把每处 svc #n / mrs cntpct 换成调用 dm_gate；这里按门块返回点（绝对地址）记住它原来是什么。
// 查法：开放寻址哈希，一次命中（原先每次门调用 module_of + 线性比对 110 项，且按模块内偏移做键，跨模块会串）。
// ---------- Host gate dispatch ----------
// The rewriter replaces every svc #n / mrs cntpct with a call to dm_gate; here we remember, by gate block return point (absolute address), what it originally was.
// Lookup: open-addressing hash, one hit (it used to do module_of + a linear compare over 110 entries on every gate call, keyed by in-module offset, which would collide across modules).
enum { G_SVC, G_CNTPCT, G_CNTVCT, G_OBS };
// 观测门（改写器 OBS 表离线插入）：编号 → 只读回调；未挂回调时门进出一次即返回
// Observation gates (inserted offline from the rewriter's OBS table): number → read-only callback; with no callback attached the gate returns after one pass in and out
static void (*g_obs_fn[OBS_MAX])(const uint64_t *regs); static int g_obs_have[OBS_MAX];
void gate_obs_set(int id, void (*fn)(const uint64_t *regs)) { if (id <= 0 || id >= OBS_MAX) die("观测门编号越界 %d", id); g_obs_fn[id] = fn; }
int gate_obs_present(int id) { return id > 0 && id < OBS_MAX && g_obs_have[id]; }
static uint8_t g_svc_have[256];   // 产物里登记过的 svc 号（剪口引入的宿主调用号 0x80 起靠它判在不在） | svc numbers registered in the build output (used to tell whether the host call numbers from 0x80 up, introduced by cuts, are present)
int gate_svc_present(int num) { return num >= 0 && num < 256 && g_svc_have[num]; }
typedef struct { uint64_t ret; int kind, arg; uint64_t orig; } gate_t;
#define GATE_BITS 12                     // 4096 槽，门数 ≤ 1024，装载因子 ≤ 1/4 | 4096 slots, gate count ≤ 1024, load factor ≤ 1/4
static gate_t g_gate[1u << GATE_BITS];
static int g_ngate;
static inline unsigned gate_h(uint64_t a) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - GATE_BITS)); }

void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig) {
    int k = !strcmp(kind, "svc") ? G_SVC : !strcmp(kind, "cntpct") ? G_CNTPCT : !strcmp(kind, "cntvct") ? G_CNTVCT : !strcmp(kind, "obs") ? G_OBS : -1;
    if (k < 0) die("未知门类 %s", kind);
    if (k == G_OBS) { if (arg <= 0 || arg >= OBS_MAX) die("观测门编号越界 %d", arg); g_obs_have[arg] = 1; }
    if (k == G_SVC && arg >= 0 && arg < 256) g_svc_have[arg] = 1;
    if (++g_ngate > (1 << GATE_BITS) / 4) die("gate overflow");
    unsigned i = gate_h(ret_abs);
    while (g_gate[i].ret) { if (g_gate[i].ret == ret_abs) die("门重复登记 %#llx", ret_abs); i = (i + 1) & ((1u << GATE_BITS) - 1); }
    g_gate[i] = (gate_t){ret_abs, k, arg, orig};
}

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   // hle_svc.c

uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs) {
    for (unsigned i = gate_h(ret_addr);; i = (i + 1) & ((1u << GATE_BITS) - 1)) {
        const gate_t *g = &g_gate[i];
        if (g->ret == ret_addr) switch (g->kind) {
            case G_CNTPCT: case G_CNTVCT: {
                uint64_t t = guest_ns() * 12 / 625;    // 19.2 MHz 系统计数器；原指令 mrs xN, cntpct_el0，arg = N | 19.2 MHz system counter; original instruction mrs xN, cntpct_el0, arg = N
                regs[g->arg] = t;
                return t;
            }
            case G_OBS: if (g_obs_fn[g->arg]) g_obs_fn[g->arg](regs); return 0;   // 只读寄存器；dm_gate 回写的 x0–x8 即原值 | Registers are read-only; the x0–x8 that dm_gate writes back are the original values
            default: return svc_dispatch(g->arg, regs, g->orig);
        }
        if (!g->ret) die("unknown gate at %#llx", ret_addr);
    }
}
