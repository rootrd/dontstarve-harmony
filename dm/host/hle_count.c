// DM_HLECOUNT=<输出文件，可含 %d = 进程号>（Mac 诊断，HLE 覆盖审计用）：统计每条 HLE 被客体调用的次数，退出时写 TSV「类别 名字 次数」（含 0 次）。
// 做法：装载时把客体对宿主实现的引用（强制接管 / HLE 表 / libc 直通 / 未实现桩）解析到一个计数桩：
//   ldr x16, =&计数; movz x17, #1; stadd x17, [x16]; ldr x16, =实现; br x16
// 只动调用间可破坏的 x16 / x17，参数与返回值原样。桩写在可写页里，装载结束封成只读可执行（hlecount_seal）。
// svc 按号单独计（hle_svc.c 分派入口调 hlecount_svc）。iOS 不可用（运行时写出的代码不能执行）。
// DM_HLECOUNT=<output file, may contain %d = process id> (Mac diagnostic, for HLE coverage audit): count how many times each HLE is called by the guest and write a TSV "category name count" on exit (including 0 counts).
// Method: at load time, resolve guest references to host implementations (forced takeover / HLE table / libc passthrough / unimplemented stubs) to a counting stub:
//   ldr x16, =&count; movz x17, #1; stadd x17, [x16]; ldr x16, =implementation; br x16
// Touches only x16 / x17, which may be clobbered between calls; arguments and return values pass through unchanged. Stubs are written in a writable page and sealed read-only executable when loading ends (hlecount_seal).
// svc is counted separately by number (the hle_svc.c dispatch entry calls hlecount_svc). Unavailable on iOS (code written at runtime cannot execute).
#include "dm.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#if !TARGET_OS_IPHONE
#define HC_MAX 8192
typedef struct hc_ent { const char *kind, *name; void *fn; _Atomic uint64_t n; } hc_ent;
static hc_ent g_hc[HC_MAX];
static unsigned g_nhc; static uint32_t *g_hcode; static int g_hon = -1, g_hsealed; static const char *g_hpath;
static _Atomic uint64_t g_hsvc[256];
static int hc_on(void) { if (g_hon < 0) { g_hpath = getenv("DM_HLECOUNT"); g_hon = g_hpath != NULL; } return g_hon; }
static void hc_write(void) {
    char pth[1024]; snprintf(pth, sizeof pth, g_hpath, (int)getpid());   // 路径里的 %d 换成进程号（并行多局各写一份） | %d in the path is replaced with the process id (parallel runs each write their own copy)
    if (getenv("DM_HLECOUNT_LOG")) {   // 真机诊断：计数直接走 hilog（沙箱文件从 hdc 取不出来） | Real-device diagnostics: counts go straight to hilog (sandbox files cannot be pulled out via hdc)
        LOG("HLC 入口 %u 条，非零计数（类别 名字 次数）：", g_nhc);
        for (unsigned i = 0; i < g_nhc; i++) { unsigned long long n = (unsigned long long)atomic_load(&g_hc[i].n); if (n) LOG("HLC %s %s %llu", g_hc[i].kind, g_hc[i].name, n); }
        for (int k = 0; k < 256; k++) { unsigned long long n = (unsigned long long)atomic_load(&g_hsvc[k]); if (n) LOG("HLC SVC %#x %llu", k, n); }
    }
    FILE *f = fopen(pth, "w"); if (!f) { LOG("【红】DM_HLECOUNT 写不了 %s", pth); return; }
    fprintf(f, "类别\t名字\t次数\n");
    for (unsigned i = 0; i < g_nhc; i++) fprintf(f, "%s\t%s\t%llu\n", g_hc[i].kind, g_hc[i].name, (unsigned long long)atomic_load(&g_hc[i].n));
    for (int k = 0; k < 256; k++) if (atomic_load(&g_hsvc[k])) fprintf(f, "SVC\t%#x\t%llu\n", k, (unsigned long long)atomic_load(&g_hsvc[k]));
    fclose(f); LOG("DM_HLECOUNT：%u 条入口 → %s", g_nhc, pth);
}
// ---------- DM_HLTRACE：把计数桩换成「计数 + 记参数」桩 ----------
// 形状（56 个字 = 224 字节：45 条指令 + 1 个对齐空位 + 3 个字面量 @ 46/48/50，每条机器码都用 clang+objdump 核过，
// 并且 dm/ohos/tests/hltrace_stub_test.c 在本机 aarch64 上把「8 个整型参、8 个浮点参、结构体返回 x8」三种调用跑通）：
//   先保住 x0..x8、x30 与 v0..v7（共 208 字节）——x8 是结构体返回指针、v0-v7 是浮点实参，宿主 C 记录函数会踩，必须存；
//   x0=参数保存区、x1=本入口的 g_hc 项 → 调 hc_trace_call → 逐项还原 → br 到真实现。
// 语义与计数桩一致：仍是尾跳，返回值原样。
// ---------- DM_HLTRACE: turn the counting stub into a counting + argument-recording stub ----------
// Shape (56 words = 224 bytes: 45 instructions + 1 alignment gap + literals at 46/48/50; every word verified with clang +
// objdump, and dm/ohos/tests/hltrace_stub_test.c exercises it natively for 8 integer args, 8 double args and a
// struct-return (x8) call). Saves x0..x8, x30 and v0..v7 (208 bytes -- x8 is the struct-return pointer, v0-v7 are float
// arguments; a host C callee would clobber them), calls hc_trace_call(args area, g_hc entry), restores, then branches
// to the real implementation. Tail-branch semantics unchanged: return values pass through untouched.
#define TR_STRIDE 56
static uint32_t *g_trcode; static int g_htrace = -1;
static int hc_trace(void) { if (g_htrace < 0) g_htrace = getenv("DM_HLTRACE") != NULL; return g_htrace; }
void hle_hist_call(const char *, uint64_t, uint64_t, uint64_t);   // hle.c：分线程调用环 | per-thread call ring (hle.c)
static void hc_trace_call(const uint64_t *a, hc_ent *e) {
    atomic_fetch_add(&e->n, 1);
    hle_hist_call(e->name, a[0], a[1], a[2]);
}
static void *stub_of(unsigned i) { return hc_trace() ? (void *)(g_trcode + TR_STRIDE * i) : (void *)(g_hcode + 12 * i); }
static void *trace_stub(unsigned i, void *fn) {
    if (!g_trcode) { g_trcode = mmap(NULL, HC_MAX * TR_STRIDE * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_trcode == MAP_FAILED) die("DM_HLTRACE mmap"); }
    static const uint32_t tmpl[] = {
        0xD10343FF,                                     // sub sp, sp, #208
        0xF90003E0, 0xF90007E1, 0xF9000BE2, 0xF9000FE3, // str x0..x3
        0xF90013E4, 0xF90017E5, 0xF9001BE6, 0xF9001FE7, // str x4..x7
        0xF90023E8, 0xF90027FE,                         // str x8, x30
        0x3D8017E0, 0x3D801BE1, 0x3D801FE2, 0x3D8023E3, // str q0..q3
        0x3D8027E4, 0x3D802BE5, 0x3D802FE6, 0x3D8033E7, // str q4..q7
        0x910003E0,                                     // mov x0, sp        = 参数保存区 | saved arguments
        0x58000350,                                     // ldr x16, +26 字 -> 46：本入口 g_hc 项 | this entry's g_hc item
        0xAA1003E1,                                     // mov x1, x16
        0x58000350,                                     // ldr x16, +26 字 -> 48：hc_trace_call
        0xD63F0200,                                     // blr x16
        0xF94027FE,                                     // ldr x30, [sp,#72]
        0xF94003E0, 0xF94007E1, 0xF9400BE2, 0xF9400FE3, // ldr x0..x3
        0xF94013E4, 0xF94017E5, 0xF9401BE6, 0xF9401FE7, // ldr x4..x7
        0xF94023E8,                                     // ldr x8, [sp,#64]
        0x3DC017E0, 0x3DC01BE1, 0x3DC01FE2, 0x3DC023E3, // ldr q0..q3
        0x3DC027E4, 0x3DC02BE5, 0x3DC02FE6, 0x3DC033E7, // ldr q4..q7
        0x910343FF,                                     // add sp, sp, #208
        0x580000F0,                                     // ldr x16, +7 字 -> 50：真实现 | real implementation
        0xD61F0200,                                     // br x16
        0xD503201F,                                     // nop（把字面量抬到 8 字节对齐）| lifts the literals to 8-byte alignment
    };
    uint32_t *w = g_trcode + TR_STRIDE * i;
    memcpy(w, tmpl, sizeof tmpl);
    uint64_t v = (uint64_t)&g_hc[i];  memcpy(w + 46, &v, 8);
    v = (uint64_t)&hc_trace_call;     memcpy(w + 48, &v, 8);
    v = (uint64_t)fn;                 memcpy(w + 50, &v, 8);
    return w;
}
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    if (!hc_on() || !fn) return fn;
    if (g_hsealed) { static int once; if (!once++) LOG("【红】DM_HLECOUNT：封页后还有解析（%s），此后的入口不计数", name); return fn; }
    for (unsigned i = 0; i < g_nhc; i++) if (g_hc[i].fn == fn && !strcmp(g_hc[i].name, name)) return stub_of(i);   // 同名同实现复用一个桩 | Same name and same implementation reuse one stub
    if (g_nhc == HC_MAX) die("DM_HLECOUNT 入口超过 %d", HC_MAX);
    if (!g_hcode) { g_hcode = mmap(NULL, HC_MAX * 48, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (g_hcode == MAP_FAILED) die("DM_HLECOUNT mmap"); atexit(hc_write); }
    unsigned i = g_nhc++; g_hc[i].kind = kind; g_hc[i].name = name; g_hc[i].fn = fn;
    if (hc_trace()) return trace_stub(i, fn);
    uint32_t *w = g_hcode + 12 * i;                                  // 48 字节：5 条指令 + 1 空 + 两个 8 字节字面量（偏移 0x18 / 0x20） | 48 bytes: 5 instructions + 1 empty + two 8-byte literals (offsets 0x18 / 0x20)
    w[0] = 0x58000000 | ((0x18 / 4) << 5) | 16;                      // ldr x16, [pc+0x18]   → &计数 | ldr x16, [pc+0x18]   → &count
    w[1] = 0xD2800031;                                                // movz x17, #1
    w[2] = 0xF8200000 | (17u << 16) | (16u << 5) | 31;               // stadd x17, [x16]（ldadd 目标 xzr） | stadd x17, [x16] (ldadd with destination xzr)
    w[3] = 0x58000000 | (((0x20 - 0xc) / 4) << 5) | 16;              // ldr x16, [pc+0x14]   → 实现 | ldr x16, [pc+0x14]   → implementation
    w[4] = 0xD61F0200;                                                // br x16
    w[5] = 0xD503201F;                                                // nop
    uint64_t a = (uint64_t)&g_hc[i].n, b = (uint64_t)fn; memcpy(w + 6, &a, 8); memcpy(w + 8, &b, 8);
    return w;
}
// 地址归属：崩溃现场里不在任何模块的裸地址，先认这两块桩页。
// 返回值指向轮转的静态缓冲（信号上下文可用：不分配、不加锁）。
// Address ownership: for a bare address in the crash scene that lies in no module, first check these two stub pages.
// The result points at a rotating static buffer (usable in a signal handler: no allocation, no lock).
static const char *hc_tag(const void *p) {
    static char bufs[4][48]; static unsigned rot;
    uint64_t a = (uint64_t)(uintptr_t)p;
    const char *kind = NULL; uint64_t base = 0;
    if (g_hcode && a >= (uint64_t)g_hcode && a < (uint64_t)g_hcode + (uint64_t)HC_MAX * 48) { kind = "计数桩页"; base = (uint64_t)g_hcode; }
    else if (g_trcode && a >= (uint64_t)g_trcode && a < (uint64_t)g_trcode + (uint64_t)HC_MAX * TR_STRIDE * 4) { kind = "HLTRACE桩页"; base = (uint64_t)g_trcode; }
    if (!kind) return NULL;
    char *o = bufs[(rot++) & 3];
    snprintf(o, sizeof bufs[0], "%s+%#llx", kind, (unsigned long long)(a - base));
    return o;
}
// 封页前逐条核对每个入口的「真实现」落在哪：模块代码段、宿主某个映射、或桩页；都不是就报红。
// 这条是给「把名字像函数的导入解析成了数据地址」那一类错误准备的：运行时表现为取指错误、现场极难读，这里一行红省一轮真机。
// Before sealing, check where each entry's real implementation lands: a module's code segment, some host mapping, or a stub page; anything else reports red.
// This exists for the class of bug where an import whose name looks like a function got resolved to a data address: at run time that shows as an
// instruction-fetch fault whose scene is very hard to read -- one red line here saves a device round.
static void hc_audit(void) {
    unsigned bad = 0;
    for (unsigned i = 0; i < g_nhc; i++) {
        void *fn = g_hc[i].fn; Dl_info di;
        if (module_addr_exec(fn) || (fn && dladdr(fn, &di)) || hc_tag(fn)) continue;
        if (bad < 8) LOG("【红】入口 %s/%s 的实现 %p 不在任何模块代码段、也不在任何宿主映射里", g_hc[i].kind, g_hc[i].name, fn);
        bad++;
    }
    if (bad) LOG("【红】DM_HLECOUNT：%u 个入口的实现落在非执行区（跳过去就是取指错误）", bad);
}
void hlecount_seal(void) {
    if (!hc_on() || !g_hcode) return;
    hc_audit();
    crash_set_page_tag(hc_tag);
    if (mprotect(g_hcode, HC_MAX * 48, PROT_READ | PROT_EXEC)) die("DM_HLECOUNT mprotect");
    if (g_trcode) {   // DM_HLTRACE 的桩在另一块映射里，一起封 | the DM_HLTRACE stubs live in a separate mapping, seal them together
        if (mprotect(g_trcode, HC_MAX * TR_STRIDE * 4, PROT_READ | PROT_EXEC)) die("DM_HLTRACE mprotect");
        sys_icache_invalidate(g_trcode, HC_MAX * TR_STRIDE * 4);
    }
    g_hsealed = 1;
    sys_icache_invalidate(g_hcode, HC_MAX * 48);
    LOG("DM_HLECOUNT：计数桩 %u 个已封%s", g_nhc, g_trcode ? "（DM_HLTRACE：带参数记录）" : "");
}
void hlecount_svc(int num) { if (g_hon > 0 && num >= 0 && num < 256) atomic_fetch_add(&g_hsvc[num], 1); }
void hlecount_flush(void) { if (hc_on() && g_hcode) hc_write(); }   // 优雅退出路径（SIGTERM / 场景断开）也写一份 | Also write a copy on the graceful exit path (SIGTERM / scene disconnect)
void hlecount_dump_now(void) { if (hc_on() && g_hcode) hc_write(); }   // nnMain 返回后立即转储（进程不退出时 atexit 不触发） | Dump right after nnMain returns (atexit does not fire while the process stays alive)
#else
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
void hlecount_seal(void) {}
void hlecount_svc(int num) { (void)num; }
void hlecount_flush(void) {}
void hlecount_dump_now(void) {}
#endif
