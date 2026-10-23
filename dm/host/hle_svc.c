// Horizon 内核系统调用（装 SDK 后由 SDK 运行时触发）。只实现 P1 启动链要的：内存布局 + 堆 + 查询 + 调试输出 + 计时。
// 寄存器约定：入参 w1.. / 出参 w0=Result、x1=值（GetInfo / SetHeapSize），与公开 Horizon ABI 一致。
// Horizon kernel system calls (triggered by the SDK runtime once the SDK is loaded). Only what the P1 boot chain needs is implemented: memory layout + heap + query + debug output + timing.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI.
#include "dm.h"
#include "hle.h"
#include <mach/mach.h>   // vm_read_overwrite（帧链上一跳的安全读；OHOS 走 compat/mach/mach.h）| vm_read_overwrite for the safe frame-chain read (OHOS resolves to compat/mach/mach.h)
#include <stdatomic.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

// 预留一整块客体地址空间，按 Horizon 分区（地址值直接用宿主 mmap 得到的真地址）。只占地址、不占内存。
// iOS 大小 5 GB：游戏要的是开头 1 GB（别名区、栈）+ 一整块堆（应用内存池 3285 MiB，见下面 APP_POOL），合计约 4.2 GB；
// iOS 上不带「扩展虚拟寻址」权限时一次最多能预留 6 GB（真机实读：6 GB 成、7 GB 不成），所以不能留得更宽——
// 留到 8 GB 的话，签不了那项权限的开发者账号（免费账号）出的包在这一步就退出。
// Reserve one whole block of guest address space, partitioned the Horizon way (address values are the real addresses returned by the host's mmap).
// It only takes address space, not memory.
// iOS size 5 GB: the game needs the first 1 GB (alias region, stacks) + one whole heap block (application memory pool of 3285 MiB, see APP_POOL
// below), about 4.2 GB in total; on iOS without the "extended virtual addressing" entitlement a single reservation can be at most 6 GB
// (read on a real device: 6 GB succeeds, 7 GB fails), so it cannot be made any wider --
// with an 8 GB reservation, builds from developer accounts that cannot sign that entitlement (free accounts) would exit at this step.
// 鸿蒙上没有 iOS 那 6 GB 预留上限，分区照 Horizon 摆开（下表）。
// 别名区必须装得下客体「默认堆」那一整块：Unity 系 SDK（DX 高海 Saga）的 nninitStartup 在 VAMM 开着的时候
// （GetInfo(16)!=0）把别名区当默认堆的来源——main+0x57bbe8 申请 (总量−已用) & 2MB 对齐 这一块，
// 走 nnmem_nlib_virtual_alloc → VammManager::AllocateAddressRegion，而 VammManager 的区域表就是构造时读到的
// GetInfo(2)/GetInfo(3)（sdk+0x134618 别名区地址、大小）。这里报 512 MB，它要 3.7 GB，区域表里放不下，
// 于是 AllocateAddressRegion 返错 → virtual_alloc 返 0xc → CentralHeap::Init 返错 → sdk+0x12648c 致命中止。
// On HarmonyOS there is no iOS 6 GB reservation cap, so the partitions are laid out as below.
// The alias region must be able to hold the guest's whole "default heap" block: the Unity-line SDK (DX High Sea Saga) uses the alias region
// as the source of its default heap when VAMM is on (GetInfo(16) != 0) — main+0x57bbe8 asks for (total - used) rounded down to 2 MB through
// nnmem_nlib_virtual_alloc -> VammManager::AllocateAddressRegion, and that manager's region table is exactly what it read from
// GetInfo(2)/GetInfo(3) at construction (sdk+0x134618: alias address and size). Reporting 512 MB here while it asks for 3.7 GB means the table
// cannot hold the window, so AllocateAddressRegion errors -> virtual_alloc returns 0xc -> CentralHeap::Init errors -> abort at sdk+0x12648c.
// 判平台沿用本仓库的写法：iOS 定义 __APPLE__，OHOS 端口不定义（见 dm/ohos/compat/ohos_force.h）。
// Platform test follows this repo's convention: iOS defines __APPLE__, the OHOS port does not (see dm/ohos/compat/ohos_force.h).
#if defined(__APPLE__)
#  define RESERVE    (5ull << 30)
#  define ALIAS_OFF  0x10000000ull
#  define ALIAS_CAP  0x20000000ull
#  define STACK_OFF  0x30000000ull
#  define HEAP_OFF   0x40000000ull
#else
#  define RESERVE    (12ull << 30)
#  define ALIAS_OFF  0ull
#  define ALIAS_CAP  (6ull << 30)
#  define STACK_OFF  (6ull << 30)
#  define HEAP_OFF   (8ull << 30)
#endif
static uint8_t *g_as;              // ASLR 区基址 = 整块 | ASLR region base = the whole block
static uint8_t *g_heap; static size_t g_heap_cur, g_heap_cap; static int g_heap_rejects;
// 地址段整个落在客体堆当前已开放（可读写）的范围内：这种读不用陷内核核对，直接 memcpy（diag.c tk_rd 等每拍读客体内存的地方用）
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap: such reads need no kernel trap to check, memcpy directly (used where guest memory is read every tick, such as diag.c tk_rd)
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
static uint8_t *g_alias; static size_t g_alias_cap;
static uint8_t *g_stack; static size_t g_stack_cap;
// 栈区映射往低地址多放开的余量（DM_STKSLACK=<字节>，默认 512 KiB，0 = 关掉）
// Extra room opened below each mapped stack (DM_STKSLACK=<bytes>, default 512 KiB, 0 = off)
static uint64_t g_stk_slack = 0x80000;

// alias 区缺页兜底：定义在下面映射登记表之后（要用 g_map），这里先声明给 mem_init 注册用。
// Alias demand-back: defined below the mapping registry because it needs g_map; declared here so mem_init can register it.
static int alias_demand_back(void *addr, int is_write, void *uc);

static void mem_init(void) {
    if (g_as) return;
    g_as = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest AS");
    // 固定划分（都在预留块内）：别名区、栈、堆从 HEAP_OFF 起到块尾
    // Fixed partition (all inside the reserved block): alias region, stack, heap from HEAP_OFF to the end of the block
    g_alias = g_as + ALIAS_OFF; g_alias_cap = ALIAS_CAP;
    // 栈区报多大，决定客体能给多少个线程建栈：nn::os 的 MapAliasStack 走 StackGuardManager 的
    // AddressSpaceAllocator，窗口就是 GetInfo(0xe)/(0xf) 这区间（sdk+0x137c24 里 start = 0xe 的值、end = 0xe + 0xf 的值），
    // 每建一个线程要在这儿放下一整条栈（高海 Saga 是 8 MiB），线程不退出就不还。
    // 写死 256 MiB 时实测：14 条之后 AllocateSpace 返 0 → MapAliasStack 返 Result 1 → sdk+0x1365c4 AbortImpl（退出码 42），
    // 而那 256 MiB 之外我们本来就留了 STACK_OFF..HEAP_OFF 的空闲窗口没用。iOS 那边这个差正好还是 0x10000000，行为不变。
    // How large we report the stack region decides how many threads the guest can build: nn::os's MapAliasStack allocates from
    // StackGuardManager's AddressSpaceAllocator, whose window is exactly this GetInfo(0xe)/(0xf) range (in sdk+0x137c24: start = the 0xe value, end = 0xe + 0xf),
    // and each new thread needs its whole stack here (8 MiB for High Sea Saga), never returned while the thread lives.
    // With 256 MiB hard-coded, measured: after 14 stacks AllocateSpace returns 0 → MapAliasStack gives Result 1 → AbortImpl at sdk+0x1365c4 (exit 42),
    // while the STACK_OFF..HEAP_OFF window we reserved and left unused was far bigger. On iOS the difference is still 0x10000000, so nothing changes there.
    g_stack = g_as + STACK_OFF; g_stack_cap = HEAP_OFF - STACK_OFF;
    g_heap  = g_as + HEAP_OFF;  g_heap_cap  = RESERVE - HEAP_OFF;   // 4 GB，容得下游戏那一整块约 3.2 GB 的堆 | 4 GB, enough to hold the game's single heap block of about 3.2 GB
    { const char *sl = getenv("DM_STKSLACK"); if (sl) g_stk_slack = strtoull(sl, NULL, 0); }
    dm_fault_hook_register(alias_demand_back);
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p 栈下余量 %#llx", g_as, (unsigned long long)RESERVE, g_heap, g_alias, g_stack, (unsigned long long)g_stk_slack);
}

// 宿主常驻内存（MB）：鸿蒙读 /proc/self/statm 第二列；iOS 没有 /proc，返回 -1 让日志显示「-1」，不骗人。
// Host resident memory in MB: on HarmonyOS read column 2 of /proc/self/statm; iOS has no /proc so return -1 and let the log show it, rather than lie.
static long long host_rss_mb(void) {
#if defined(__APPLE__)
    return -1;
#else
    FILE *f = fopen("/proc/self/statm", "r"); if (!f) return -1;
    long long tot = 0, rss = 0; int n = fscanf(f, "%lld %lld", &tot, &rss); fclose(f);
    return n == 2 ? rss * (long long)sysconf(_SC_PAGESIZE) / (1 << 20) : -1;
#endif
}

// 应用内存池 = Switch（4 GB 机型）给应用的 3285 MB。游戏启动（main+0x1b66160）把 QueryMemoryInfo 报的总量整块设成堆、失败就减 2 MB 重试，
// 整块交给自己的 StandardAllocator（main+0x32137a0）——所以堆大小 = 池 − 映像占用，宿主报多少它就只有多少。
// 之前这里报 3 GB 且 SetHeapSize 从不失败：堆比真机少约 150 MB，进局高峰主堆只剩约 18 MB，本机负载高时时序一偏申请 128 KB 返回空、
// 客体不判空直接写（CRASH main+0x1b36138，合入后复核 inv_tap）。
// Application memory pool = the 3285 MB the Switch (4 GB model) gives applications. At startup (main+0x1b66160) the game sets the whole total reported by QueryMemoryInfo as the heap, retrying with 2 MB less on failure,
// and hands the whole block to its own StandardAllocator (main+0x32137a0) -- so heap size = pool − image footprint, it has only as much as the host reports.
// Previously 3 GB was reported here and SetHeapSize never failed: the heap was about 150 MB smaller than on the real device, at the run entry peak the main heap had only about 18 MB left, and under high local load a timing shift made a 128 KB request return null,
// which the guest writes to without a null check (CRASH main+0x1b36138, re-verified after merge with inv_tap).
#define APP_POOL 0xCD500000ull
// 报给游戏的池 = Switch 的 3285 MB + 512 MB（DM_APP_POOL_ADD_MB=<整数> 可调，可为负——往小调用来造「主堆用尽」的现场）。
// 为什么多给：游戏自己固定划走约 3.08 GB，主堆余量进局只有 100–180 MB、换图时地板三四十 MB、最大可分配块二十几 MB；用尽时原版多处不判空直接写
// （公开仓库，另见下面 dm_guest_heap_stat）。实测多给的量一比一变成主堆余量（138 → 649 MB），游戏划走的不跟着变；
// 没碰过的堆页不计入常驻内存，余量真被用掉时常驻才涨。堆区上限是预留里的 4 GB（mem_init），3728 MB 放得下；再加要先动预留。
// 取 +512 而不是更大：给 6 GB 内存的机型留余地，真机上限有读数后再定。
// Pool reported to the game = the Switch's 3285 MB + 512 MB (adjustable with DM_APP_POOL_ADD_MB=<integer>, may be negative -- lowering it is how to stage a "main heap exhausted" scene).
// Why more is given: the game carves out a fixed 3.08 GB or so for itself, leaving the main heap only 100–180 MB of free space on entering a game, a floor of thirty to forty MB when changing maps and a largest allocatable block of twenty-odd MB; when it runs out the original game writes through null without checking in many places
// (see also dm_guest_heap_stat below). Measured: the extra amount turns one-to-one into main heap free space (138 -> 649 MB), while what the game carves out does not change with it;
// heap pages never touched do not count toward resident memory, which grows only when the free space is actually used. The heap region limit is the 4 GB in the reservation (mem_init), which fits 3728 MB; adding more means changing the reservation first.
// +512 rather than more: to leave room for devices with 6 GB of RAM; to be revisited once there are readings of the limit on real devices.
static uint64_t app_pool(void) { static uint64_t v; if (!v) { const char *e = getenv("DM_APP_POOL_ADD_MB"); long add = e ? atol(e) : 512; v = APP_POOL + ((uint64_t)add << 20); if (e) LOG("DM_APP_POOL_ADD_MB：应用内存池报 %llu MB（Switch 的 3285 MB %+ld）", (unsigned long long)(v >> 20), add); } return v; }
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) n += g_mod[i].size; return (n + 0x1fffff) & ~(size_t)0x1fffff; }

// 游戏主堆读数：主堆 = main+0x32137a0 那个 nn::mem::StandardAllocator（启动时把整块堆交给它）。申请失败返回空，原版多处不判空直接写
// （公开仓库 连战 82 分钟后异步读盘回调 main+0x27ecdc 要 765 KB 连续块得空，memset 写 0 地址；进程常驻内存看不出这件事）。
// 余量 / 最大可分配块调的是分配器自己的查询（sdk+0x2f9d78 / +0x2f9dbc，内部走 CentralHeap::Query、自己加锁），只许在客体线程上调。
// 读到的数留一份在 g_gheap_last 里，分配返回空时的报红直接带上次读数（那一刻不再进分配器）。单位：总量 / 余量 MB，最大块 KB
// Game main heap readout: the main heap = the nn::mem::StandardAllocator at main+0x32137a0 (the whole heap block is handed to it at startup). A failed allocation returns null, and the original game writes through it without a null check in many places
// (after 82 minutes of boss rush, the asynchronous disk-read callback main+0x27ecdc asked for a contiguous 765 KB block, got null, and memset wrote to address 0; the process resident memory does not show this).
// Free space / largest allocatable block call the allocator's own queries (sdk+0x2f9d78 / +0x2f9dbc, which go through CentralHeap::Query and take their own lock) and may only be called on a guest thread.
// The values read are kept in g_gheap_last, and the red report on a null allocation carries the last readout directly (the allocator is not entered again at that moment). Units: total / free in MB, largest block in KB
static module_t *mod_named(const char *n) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, n)) return &g_mod[i]; return NULL; }
static _Atomic unsigned long long g_gheap_last[3];
int dm_guest_heap_stat(unsigned long long *total_mb, unsigned long long *free_mb, unsigned long long *maxblk_kb) {
    module_t *mm = mod_named("main"), *ms = mod_named("sdk"); if (!mm || !ms || !g_heap_cur) return 0;
    uint8_t *al = mm->base + 0x32137a0; if (!al[0]) return 0;   // 首字节 = 已初始化（sdk+0x2f9898 写） | First byte = initialized (written by sdk+0x2f9898)
    uint64_t fr = ((uint64_t (*)(void *))(ms->base + 0x2f9d78))(al), mx = ((uint64_t (*)(void *))(ms->base + 0x2f9dbc))(al);
    *total_mb = g_heap_cur >> 20; *free_mb = fr >> 20; *maxblk_kb = mx >> 10;
    g_gheap_last[0] = *total_mb; g_gheap_last[1] = *free_mb; g_gheap_last[2] = *maxblk_kb; return 1;
}
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEmm", "照原版分配，只在返回空时报红（原版调用方多处不判空）", void *, hle_stdalloc_allocate, void *self, size_t size, size_t align) {
    static void *(*orig)(void *, size_t, size_t);
    if (!orig) {
        // 原版实现按「符号名」在客体 sdk 里取：D2R 的 AllocateEmm 在 sdk+0x2f9974，饥荒的在 sdk+0x2f1174，
        // 写死偏移在换游戏时就打到别的函数上（饥荒实测从这里飞：sdk+0x2f9974 落在 nn::mii::detail::CoreDataRaw::Verify 里，
        // 拿 (this,size,align) 当自己的参数跑完 ret 到垃圾地址）。D2R 用符号取到的就是 0x2f9974，行为逐字不变。
        // Resolve the original by symbol name inside the guest sdk: D2R's AllocateEmm is at sdk+0x2f9974,
        // Don't Starve's at sdk+0x2f1174; a hard-coded offset lands in another function for a different game.
        void *module_sym(module_t *m, const char *name);
        module_t *ms = mod_named("sdk");
        void *sym = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEmm") : NULL;
        if (sym) { orig = (void *(*)(void *, size_t, size_t))sym; LOG("StandardAllocator::Allocate 原版实现（按符号）= %p", sym); }
        else if (ms) { orig = (void *(*)(void *, size_t, size_t))(ms->base + 0x2f9974); LOG("【红】StandardAllocator::Allocate 符号未命中，退回 D2R 偏移 %p", (void *)orig); }
        else { LOG("【红】没有 sdk 模块，StandardAllocator::Allocate 无法转调原版"); return NULL; }
    }
    void *p = orig(self, size, align);
    if (!p && size) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) LOG("【红】游戏堆分配返回空：申请 %zu 字节（对齐 %zu），分配器 %p；上次读数 主堆 %llu MB、余量 %llu MB、最大可分配块 %llu KB", size, align, self,
        (unsigned long long)g_gheap_last[0], (unsigned long long)g_gheap_last[1], (unsigned long long)g_gheap_last[2]); }
    return p;
}

// 内存账（memstat.m）用：客体地址空间整块范围（未初始化时 base = NULL）
// For memory accounting (memstat.m): the range of the whole guest address space block (base = NULL when not initialized)
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = RESERVE; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23 };

static uint8_t g_seen[256];

// 已映射区间登记（MapMemory 栈、MapPhysicalMemory alias 页），QueryMemory 据此回报；SDK 靠它找空位
// Registry of mapped ranges (MapMemory stacks, MapPhysicalMemory alias pages), QueryMemory reports from it; the SDK relies on it to find free space
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {       // 返回 1 基下标，0 = 无重叠 | returns a 1-based index, 0 = no overlap
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {
    pthread_mutex_lock(&g_map_lock);
    if (g_nmap >= MAX_MAP) die("映射表满");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
    pthread_mutex_unlock(&g_map_lock);
}
static void map_del(int i) { pthread_mutex_lock(&g_map_lock); g_map[i] = g_map[--g_nmap]; pthread_mutex_unlock(&g_map_lock); }

// alias 区缺页兜底。alias 区在预留块里天生是 PROT_NONE，客体要哪页我们才 mprotect 哪页（SVC 0x2C）。
// Unity 系（高海 Saga DX）不守这个规矩：它按 2 GB 对齐划设备堆，块基址直接当对象指针使，却只对用过的页发
// 映射请求——v14 实测线程「UnityGfxDeviceWorker」拿 x0=0x6b00000000 读 +0x24d0，而整段
// [0x6b00000000, 0x6b64c80000) 一次映射请求都没发过（DM_SVC=1 那轮收到 121 条，全在洞的两侧）。真机 VAMM
// 在建块时就把物理页绑好，那边读得动。这里补上这一步：按需把该页设成可读写零页再继续。
// 只在「本来就要如实报崩」的路上触发：钩子排在缺页钩子链里，不属于 alias 区的地址一律返回 0 交下去。
// Alias demand-back. The alias region inside the reservation is PROT_NONE by construction and a page only became RW
// when the guest asked for it (SVC 0x2C). The Unity line does not honour that -- it carves the device heap on 2 GB
// boundaries and uses a block base straight as an object pointer, yet requests pages only for the part it touches:
// measured in v14, the thread 「UnityGfxDeviceWorker」 dereferenced x0=0x6b00000000 at +0x24d0 while
// [0x6b00000000, 0x6b64c80000) never got one request (the DM_SVC=1 run logged 121, all on either side of the hole).
// Real-hardware VAMM binds the physical pages when it creates the block, so that read works there; this supplies the
// missing step -- back the page as zeroed read/write memory and carry on. It only fires on a path that was about to
// report a crash anyway: an address outside the alias region returns 0 and falls through to the next hook.
static int alias_demand_back(void *addr, int is_write, void *uc)
{
    (void)uc;
    uint64_t lo = (uint64_t)g_alias, hi = lo + g_alias_cap, a = (uint64_t)(uintptr_t)addr;
    if (!g_alias || a < lo || a >= hi) return 0;
    uint64_t p = a & ~(uint64_t)0x1FFFFF, n = 0x200000;   // 2 MB 一页，贴边时退到 4 KiB | 2 MB granularity, 4 KiB at the edge
    if (p < lo) { p = a & ~(uint64_t)0xFFF; n = 0x1000; }
    if (p + n > hi) n = hi - p;
    if (mprotect((void *)p, (size_t)n, PROT_READ | PROT_WRITE) != 0) return 0;
    if (pthread_mutex_trylock(&g_map_lock) == 0) {   // 登记：QueryMemory 不能再把这个区间当成空闲 | so QueryMemory stops reporting it free
        if (g_nmap < MAX_MAP) g_map[g_nmap++] = (typeof(g_map[0])){ p, n, 0, 0x0C /*MappedMemory*/ };
        pthread_mutex_unlock(&g_map_lock);
    }
    static _Atomic unsigned nb;
    unsigned k = atomic_fetch_add(&nb, 1);
    if (k < 24) LOG("alias 按需供页 %p+%#llx（缺页 %p，客体在【%s】）", (void *)p, (unsigned long long)n, addr,
                    is_write ? "写" : "读");
    else if (k == 24) LOG("alias 按需供页 已累计 %u 页，后续不再逐条打印", k);
    return 1;
}

int svc_thread(int num, uint64_t *regs);   // hle_thread.c：线程与同步类 | hle_thread.c: thread and synchronization kind

// 诊断：睡眠/等待类 svc 打「客体调用者」。依据：门块公共段是 stp x29,x30,[sp,#-16]! + blr，
// 所以宿主帧里的 regs[29]/regs[30] 就是原客的 x29/x30；x30 = 那次 bl 的返回点 = 调用者地址（模块+偏移可直接反汇编），
// x29+8 = 再上一跳的返回点候选（帧指针是否有效不赌：读失败或不在模块内就留空）。
// 用途：钉死 nnMain 里那 1 秒「SleepThread(10ms) 轮询」是谁在等、等的是哪个句柄/地址。
// Diagnostic: print the guest caller for sleep/wait svc calls. The gate block's common prologue is
// "stp x29,x30,[sp,#-16]! + blr", so the host frame's regs[29]/regs[30] hold the guest's original x29/x30:
// x30 is the return address of the bl that reached the syscall stub (= caller, symbolizable to module+offset),
// x29+8 is the next hop up. No guessing about frame pointers: an unreadable or out-of-module value prints as "-".
static void svc_caller_diag(int num, const uint64_t *regs, uint64_t orig_addr) {
    static uint64_t seen_lr[24]; static unsigned seen_n[24]; static int seen_cnt;
    static const char *nm[] = { [0x0B] = "SleepThread", [0x11] = "SignalEvent", [0x18] = "WaitSync",
                                [0x1C] = "WaitPWK", [0x1D] = "SignalPWK" };
    uint64_t lr = regs[30];
    int i, slot = -1;
    for (i = 0; i < seen_cnt; i++) if (seen_lr[i] == lr) { slot = i; break; }
    if (slot < 0) {
        if (seen_cnt >= (int)(sizeof seen_lr / sizeof seen_lr[0])) return;   // 调用点太多：不再记录 | too many distinct callers: stop recording
        slot = seen_cnt; seen_lr[seen_cnt++] = lr;
    }
    unsigned n = __atomic_fetch_add(&seen_n[slot], 1, __ATOMIC_RELAXED);
    // 长睡（>=100 ms）单独放行：帧限流器一旦把时钟算错就会睡到天荒地老，而「每调用点前 3 次」的额度早被 10 ms 轮询吃光，
    // 这类必须一直看得见（每 64 次打一条，避免刷屏）。
    // Let long sleeps (>=100 ms) through separately: a frame limiter with a bad clock sleeps forever, but the first-3-per-site
    // budget is already spent on 10 ms polling, so these have to stay visible (one line every 64 hits).
    int big = num == 0x0B && regs[0] >= 100000000ULL;
    // 进程宽键等待（0x1C/0x1D）也留一条常流水：前端冻死时十几条客体线程全停在 0x1C，只看前 3 次的话
    // 稳态恰好落在额度之外，等于没有探针。每 1024 次放一条，够认出「谁在等哪个键、等了多久」。
    // Leave a trickle for the process-wide-key waits (0x1C/0x1D): when the front end froze, a dozen guest threads all sat
    // in 0x1C, and with only the first 3 hits per site the steady state falls outside the budget, i.e. no instrument at all.
    // One line every 1024 hits is enough to see who waits on which key, and for how long.
    int pwk = (num == 0x1C || num == 0x1D) && n >= 3 && (n - 3) % 1024 == 0;
    if (n >= 3 && !(big && (n - 3) % 64 == 0) && !pwk) return;   // 每个调用点前 3 次（栈链已够长，省日志量）| first 3 hits per call site
    char chain[420]; chain[0] = 0; size_t used = 0;
    uint64_t fp = regs[29]; int lvl = 0;
    while (fp && lvl < 6) {
        uint64_t ret = 0, nxt = 0; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), fp + 8, 8, (vm_address_t)&ret, &got) != 0 || got != 8) break;
        if (vm_read_overwrite(mach_task_self(), fp, 8, (vm_address_t)&nxt, &got) != 0 || got != 8) break;
        module_t *m = module_of((void *)ret);
        char cell[56];
        if (m) snprintf(cell, sizeof cell, "%s%s+%#llx", lvl ? " <- " : "", m->name, (unsigned long long)(ret - (uint64_t)m->base));
        else snprintf(cell, sizeof cell, "%s%#llx", lvl ? " <- " : "", (unsigned long long)ret);
        if (used + strlen(cell) + 2 >= sizeof chain) break;
        strcat(chain, cell); used += strlen(cell);
        if (nxt <= fp || nxt - fp > 0x10000) break;   // 帧链必须单调向上，否则是数据冒充 | frames must ascend, else it's data pretending
        fp = nxt; lvl++;
    }
    LOG("[svc] %s #%u x0=%#llx x1=%#llx x2=%#llx x3=%#llx 帧链=[%s] 门=%#llx",
        nm[num] ? nm[num] : "svc", n, (unsigned long long)regs[0], (unsigned long long)regs[1],
        (unsigned long long)regs[2], (unsigned long long)regs[3], chain, (unsigned long long)orig_addr);
}

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    // 每线程「最后一次 SVC 号」：采样器直接打出来，用来分辨「在等」和「在某个 SVC 里空转」
    // Per-thread last SVC number: printed by the sampler to tell "blocked" from "spinning inside an SVC".
    { extern void thr_dump_maybe_start(void); thr_dump_maybe_start(); }
    { extern void spur_maybe_start(void); spur_maybe_start(); }
    { extern _Atomic int g_last_svc[256]; extern _Atomic uint64_t g_last_svc_a0[256]; extern unsigned thr_cur_slot(void);
      unsigned sl = thr_cur_slot() & 0xff;
      atomic_store_explicit(&g_last_svc[sl], num, memory_order_relaxed);
      atomic_store_explicit(&g_last_svc_a0[sl], regs[0], memory_order_relaxed); }
    { void hlecount_svc(int); hlecount_svc(num); }   // DM_HLECOUNT：svc 按号计数 | DM_HLECOUNT: svc counts by number
    if (num == 0x0B || num == 0x11 || num == 0x18 || num == 0x1C || num == 0x1D) svc_caller_diag(num, regs, orig_addr);
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    { uint64_t svc_host_call(int, uint64_t *); if (num >= 0x80 && svc_host_call(num, regs)) return regs[0]; }   // 0x80 起：剪口约定的宿主调用（hle_tact.c） | from 0x80: host calls by cut convention (hle_tact.c)
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD401; return 0; }
        if (size + image_bytes() > app_pool()) { g_heap_rejects++; regs[0] = 0x10801; return 0; }   // 超出应用内存池：ResultLimitReached。游戏启动时按 2 MB 递减重试直到放得下（main+0x1b66184） | exceeds the application memory pool: ResultLimitReached. At startup the game retries in 2 MB decrements until it fits (main+0x1b66184)
        int rc = 0;
        if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) rc = mprotect(g_heap + size, g_heap_cur - size, PROT_NONE);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        { static int said; if (!said && size) { said = 1; LOG("SetHeapSize：首次设堆成功 %zu MB（应用内存池 %llu MB − 映像 %zu MB；此前被拒 %d 次）", size >> 20, (unsigned long long)(app_pool() >> 20), image_bytes() >> 20, g_heap_rejects); } }
        g_heap_cur = size;
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: regs[0] = 0; return 0;   // SetMemoryPermission
    case 0x03: regs[0] = 0; return 0;   // SetMemoryAttribute
    case 0x04: {  // MapMemory(dst, src, size)：SDK 建线程时把堆上的栈搬进栈区 | MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        if ((uint8_t *)dst < g_stack || (uint8_t *)dst + size > g_stack + g_stack_cap || map_find(dst, size)) {
            LOG("MapMemory 目标非法或已占用 %#llx+%#llx", dst, size); regs[0] = 0xDC01; return 0; }
        // 苹果页 16K、Horizon 页 4K：可访问区扩到 16K 边界（多放开的是同区未用页，不影响语义）
        // Apple pages are 16K, Horizon pages 4K: the accessible region is extended to a 16K boundary (the extra pages opened are unused pages of the same region, semantics unaffected)
        // 下面再多放一段余量：客体线程栈只有 0x14000（80 KiB），而 HLE 门与被调进的宿主 libc 帧比 Switch 原生占用多，
        // 真机实测某个文件线程往下跑穿 213 KiB（sp = 栈底 − 0x341C0，三次跑同一个偏移），一头撞进栈区里还没放开的洞 → SEGV_ACCERR 整个进程没。
        // 余量落在同一个预留块的空洞里，不额外占内存；栈只往下长，所以余量只放在下面。代价：真写穿了不再立刻报错，最多多跑 512 KiB 才撞。
        // Open extra room further down: guest thread stacks are only 0x14000 (80 KiB), while the HLE gates and the host libc functions called through them take more than the native Switch ones;
        // measured on device a file thread ran 213 KiB past the bottom (sp = stack base − 0x341C0, the same offset in three runs) and fell into an unopened hole in the arena → SEGV_ACCERR took the whole process down.
        // The slack lies inside blank space of the same reservation, so it costs no memory; stacks only grow down, so the slack goes below. The price: a genuine overrun no longer faults at once, it gets 512 KiB more before it hits.
        uint64_t lo = dst - g_stk_slack > (uint64_t)g_stack ? ((dst - g_stk_slack) & ~0x3FFFull) : (uint64_t)g_stack;
        uint64_t hi = (dst + size + 0x3FFF) & ~0x3FFFull;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        memcpy((void *)dst, (void *)src, size);   // 搬移语义：映射期间源区不被访问（Horizon 把它设为不可访问） | move semantics: the source region is not accessed while mapped (Horizon makes it inaccessible)
        map_add(dst, size, src, 0x0B /*MappedMemory*/);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size)：内容搬回源区；16K 页可能与相邻映射共用，不收回权限 | UnmapMemory(dst, src, size): contents are moved back to the source region; a 16K page may be shared with an adjacent mapping, permissions are not revoked
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            LOG("UnmapMemory 与映射不符 %#llx+%#llx", dst, size); regs[0] = 0xD401; return 0; }
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        int k;
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (a >= g_as && a < g_as + RESERVE) {   // 空闲：到相邻映射/堆边界为止 | free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + RESERVE;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else {
            // 地址落在「当前宿主线程的栈」里：回报整段栈（状态 0xB = 栈）。SDK 初始化主线程记录时拿当时的 sp 查这里，把回报的区间当主线程栈；
            // 之后 nn::os::GetCurrentStackInfo 会断言 sp 仍在那个区间里，不在就中止。原先对客体地址空间之外的地址一律回报「它所在的 4 KiB 页」，
            // 主线程栈于是被记成一页——初始化时的 sp 与后来的 sp 是否同页，取决于宿主栈顶的绝对位置，而它随环境变量 / 启动参数总长移动（换个环境偶发启动即中止）。
            // 暗黑这一版不调 GetCurrentStackInfo（main / subsdk0 没有导入它，SDK 内部也没有调用点；宿主垫 12 档环境变量长度都起得到主菜单），所以没踩到；回报「一页」本身是错的，照改。
            // The address lies in "the current host thread's stack": report the whole stack (state 0xB = stack). When the SDK initializes the main thread record it queries here with the sp at that moment and takes the reported range as the main thread stack;
            // later nn::os::GetCurrentStackInfo asserts that sp is still inside that range and aborts if not. Previously every address outside the guest address space was reported as "the 4 KiB page containing it",
            // so the main thread stack was recorded as one page. Whether the sp at init and a later sp share a page depends on the absolute position of the host stack top, which moves with the total length of environment variables / launch arguments (a different environment occasionally aborts at startup).
            // This Diablo build does not call GetCurrentStackInfo (main / subsdk0 do not import it and the SDK has no internal call site; the host reaches the main menu with 12 different environment padding lengths), so it was never hit; reporting "one page" is wrong in itself, so fix it anyway.
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            else { base=addr & ~0xFFFull; size=0x1000; state=3/*Code*/; perm=5; } }
        if (mi) { uint64_t *m=(uint64_t*)mi; m[0]=base; m[1]=size; ((uint32_t*)mi)[4]=state; ((uint32_t*)mi)[5]=0; ((uint32_t*)mi)[6]=perm; ((uint32_t*)mi)[7]=0; ((uint32_t*)mi)[8]=0; }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size)：只许在 alias 区内（SDK 的 VammManager 用） | MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uint8_t *a = (uint8_t *)regs[0]; size_t size = regs[1];
        if (a < g_alias || a + size > g_alias + g_alias_cap) { LOG("SVC %#x 越出 alias 区 %p+%#zx", num, a, size); regs[0] = 0xD401; return 0; }
        int rc = num == 0x2C ? mprotect(a, size, PROT_READ | PROT_WRITE)
                             : (madvise(a, size, MADV_FREE), mprotect(a, size, PROT_NONE));
        if (rc) die("SVC %#x mprotect %p+%#zx errno=%d", num, a, size, errno);
        // alias 水位 + 宿主常驻：v20 把这台中止钉到「栈区窗口」而不是 alias 区——客体的 8 MiB 线程栈落在 GetInfo(0xe)/(0xf)
        // 报的那段里，256 MiB 只够 14 条，栈区放大后接着要盯的是宿主常驻（每条栈被 memcpy 写过就是 8 MiB 实页）。
        // Alias water level plus host residency: v20 pinned this abort to the "stack region window", not the alias region -- the guest's 8 MiB
        // thread stacks live in what GetInfo(0xe)/(0xf) reports, and 256 MiB only fits 14 of them. With the stack region widened, the number to
        // watch next is host residency (a stack once written by the memcpy is 8 MiB of real pages).
        static _Atomic long long use; static _Atomic unsigned nm, nu;
        long long d = num == 0x2C ? (long long)size : -(long long)size;
        long long u = atomic_fetch_add(&use, d) + d;
        atomic_fetch_add(num == 0x2C ? &nm : &nu, 1);
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx（alias 水位 %lld MB，映射 %u 段 / 卸载 %u 次，宿主常驻 %lld MB）", num == 0x2C ? "Map" : "Unmap", a, size,
                            u >> 20, atomic_load(&nm), atomic_load(&nu), host_rss_mb());
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size)：SDK 致命中止 | Break(reason, ptr, size): SDK fatal abort
        unsigned char mc0;
        uint32_t res = (regs[1] && regs[2] >= 4 && !mincore((void *)((uintptr_t)regs[1] & ~0xFFFull), 4096, &mc0)) ? *(uint32_t *)regs[1] : 0;
        // B28 实测：AbortImpl 这条路进来的 reason=0、size 在 4~7 之间，字符串一条也没打出来（下面的循环要 size>=8），
        // 只拿到 Result=0x1a80a。缺的是「谁在什么断言上失败」——x0..x3 原样导一份，另外把 regs[1] 既当指针也当值看。
        // Measured on B28: this path arrives with reason=0 and size between 4 and 7, so not one string got printed (the
        // loop below requires size>=8) and all we got was Result=0x1a80a. What is missing is which assertion failed --
        // dump x0..x3 as they are, and read regs[1] both as a pointer and as the value itself.
        LOG("Break 参数 x0=%#llx x1=%#llx x2=%#llx x3=%#llx（按值看 x1=%#llx）",
            (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2],
            (unsigned long long)regs[3], (unsigned long long)regs[1]);
        // 客体 SDK 的致命中止会带一组字符串（文件/函数/表达式/结果码）：直接把 x1 指的这块和解出来的串打出来，
        // 这是「它为什么自杀」的第一手证词。
        // The guest SDK's fatal abort carries a set of strings (file / function / expression / result): dump the block
        // x1 points at and resolve the strings -- first-hand testimony of why it killed itself.
        // 字符串扫描不再受 size 限制：这是致命路径，读过头也不会更糟（崩溃现场同样把这块打出来），而断言文本
        // 常常就在 Result 后面的同一个块里。
        // The string scan no longer depends on size: this is the fatal path, so reading past the block cannot make
        // things worse (the crash dump prints the same area), and the assertion text usually sits right after the
        // Result in one block.
        if (regs[1] > 0x1000 && regs[1] < 0x800000000000ull) {
            // 先验页映射：这块是「诊断用的随手读」，块里的字和字指向的串都可能是野指针——
            // 第 133 轮实测 Break 现场里一个字的低位 0x1015 被当成指针，宿主读它直接 SIGSEGV，把客体的断言文本盖掉了。
            // Verify the page is mapped first: this block read is a best-effort diagnostic, and both the words and the
            // strings they point at may be wild. Round 133: a word whose low half was 0x1015 got read as a string pointer,
            // the host took a SIGSEGV, and the guest's assertion text was lost.
            unsigned char mc;
            if (mincore((void *)((uintptr_t)regs[1] & ~0xFFFull), 4096, &mc)) {
                LOG("Break 信息块 %#llx 未映射，跳过", (unsigned long long)regs[1]);
            } else {
            const uint64_t *w = (const uint64_t *)(uintptr_t)regs[1];
            unsigned nw = 8;
            for (unsigned i = 0; i < nw; i++) {
                uint64_t v = w[i];
                const char *sp2 = (v > 0x10000 && v < 0x800000000000ull) ? (const char *)(uintptr_t)v : NULL;
                char sb[128]; sb[0] = 0;
                if (sp2 && !mincore((void *)((uintptr_t)sp2 & ~0xFFFull), 4096, &mc)) {
                    size_t room = 0x1000 - ((uintptr_t)sp2 & 0xFFF), k = 0;
                    while (k < 120 && k < room && sp2[k] >= 0x20 && sp2[k] < 0x7f) { sb[k] = sp2[k]; k++; }
                    sb[k] = 0;
                }
                LOG("Break 信息[%u] = %#llx  %s", i, (unsigned long long)v, sb[0] ? sb : "");
            }
            }
        }
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc 位于 %s+%#llx  调用者=%#llx", ++nb, regs[0], res, mm ? mm->name : "?", orig_addr, (unsigned long long)regs[30]);
        // callee-saved 那一批要单独打：sdk 的断言尾巴（VAbortImpl → svc Break）只用 x0..x3，
        // 出问题的参数早在调用方手里（x19、x20…），lr/x29 链又只给地址不给值。
        // The tail needs the callee-saved set on its own: the SDK's abort tail (VAbortImpl -> svc Break) uses only x0..x3,
        // while the offending arguments still live in the caller's x19/x20/... and the lr/x29 chain gives addresses, not values.
        LOG("Break 现场 x19=%#llx x20=%#llx x21=%#llx x22=%#llx x23=%#llx",
            (unsigned long long)regs[19], (unsigned long long)regs[20], (unsigned long long)regs[21],
            (unsigned long long)regs[22], (unsigned long long)regs[23]);
        LOG("Break 现场 x24=%#llx x25=%#llx x26=%#llx x27=%#llx x28=%#llx x29=%#llx x30=%#llx",
            (unsigned long long)regs[24], (unsigned long long)regs[25], (unsigned long long)regs[26],
            (unsigned long long)regs[27], (unsigned long long)regs[28],
            (unsigned long long)regs[29], (unsigned long long)regs[30]);
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // 仅通知，可继续 | notification only, can continue
        // 退出前把 stderr 冲干再等桥线程读完：stderr→hilog 是另一条线程，直接 _exit 会把 Break 与它前面那几行一起带走
        // Flush stderr and let the bridge drain before exiting: the stderr->hilog bridge is another thread,
        // so a bare _exit takes the Break line and the few lines before it away with it.
        fflush(stderr); fflush(stdout);
        usleep(400000);
        // 中止同样是一次现场：把崩溃路径那两件套（NVN 调用普查 + 客体线程清单）也导一份。
        // 普查一条就 4 KB、线程清单每线程一条长行，走 stderr→hilog 桥会被整条吞掉（v19 的两次中止什么都没留下），
        // 所以先把 fd 2 改道到 $DM_CRASHDIR/break.txt 再打。
        // An abort is a scene too: export the same two items the crash path exports (the NVN call census + the guest thread list).
        // The census is one 4 KB line and the thread list one long line per thread, and the stderr->hilog bridge swallows such lines whole
        // (the two aborts in v19 left nothing behind), so redirect fd 2 to $DM_CRASHDIR/break.txt before printing.
        { const char *d = getenv("DM_CRASHDIR"); if (!d || !*d) d = getenv("DM_SAVE");
          if (d && *d) { char p[512]; snprintf(p, sizeof p, "%s/break.txt", d);
              int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) { dup2(fd, 2); close(fd); } } }
        { extern void nvn_dump_counts(const char *); nvn_dump_counts("中止时"); }
        { extern void svc_thread_dump(void); svc_thread_dump(); }
        fflush(stderr);
        _exit(42);
    }
    case 0x1E: regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        // 真机诊断：客体 SDK 的调试串就是要不要继续跑的第一手证词，直接进 hilog（前 200 条，不可打印字符转 '.'）
        // Real-device diagnostic: the guest SDK's debug strings are the primary testimony on whether it wants to keep running, so send them straight to hilog (first 200, non-printables become '.')
        { static unsigned ns; if (ns < 200 && regs[0] && regs[1] >= 2 && regs[1] <= 512) {
              char buf[520]; uint64_t n = regs[1]; ns++;
              for (uint64_t i = 0; i < n; i++) { char c = ((char *)(uintptr_t)regs[0])[i]; buf[i] = (c >= 32 && c < 127) ? c : (c == '\n' ? ' ' : '.'); }
              buf[n] = 0; LOG("[guest] %s", buf); } }
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = RESERVE;     break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = app_pool(); break;
        case I_UsedMem:  case I_UsedNonSys:  v = g_heap_cur + image_bytes(); break;   // 已用 = 堆 + 映像，与 SetHeapSize 的核对同一口径（原先写死 64 MB）。sdk 的 QueryMemoryInfo（sdk+0x34f4ec）只拿它算「程序占用」那一格； | Used = heap + image, the same accounting as the check in SetHeapSize (it used to be hard-coded to 64 MB). The sdk's QueryMemoryInfo (sdk+0x34f4ec) only uses it to compute the "program usage" field;
                                                 // 「可用总量」那一格直接是池总量，所以游戏启动设堆仍要从整池起按 2 MB 往下减到 池 − 映像（约 34 次被拒）——这是原版在真机上的走法，不是我们的口径错
                                                 // The "total available" field is simply the pool total, so at startup the game still has to step down from the whole pool by 2 MB at a time until pool - image (about 34 refusals) when setting its heap -- this is how the original behaves on the real device, not a mistake in our accounting
        case I_SysResTotal: v = 0x2000000; break; case I_SysResUsed: v = 0x400000; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t dm_entropy64(void); v = dm_entropy64(); break; }   // DM_DETCLOCK 开时为确定流（hle_os.c） | a deterministic stream when DM_DETCLOCK is on (hle_os.c)
        case I_ProgramId: {   // 按环境的标题号（DM_PROGRAM_ID=0x...），缺省仍是暗黑2 —— 换游戏必须给对：SDK 会拿它做按标题的查表/路径 | title id from the environment (DM_PROGRAM_ID=0x...), default stays Diablo II; a different game needs the right one
            static uint64_t pid; if (!pid) { const char *e = getenv("DM_PROGRAM_ID"); pid = e && *e ? strtoull(e, 0, 0) : (uint64_t)0x0100726014352000ull; LOG("GetInfo(ProgramId) = %#llx%s", (unsigned long long)pid, e ? "（环境）" : "（缺省 暗黑2）"); } v = pid; break; }
        case I_IsApplication: v = 1; break;
        case I_UserExcAddr: v = 0; break;
        default: v = 0; break;
        }
        // DM_INFOLOG=1：每一次 GetInfo 都报「问的哪一格、答了多少」。换游戏第一轮必开：
        // 客体拿这些数算自己的默认堆、内存池，算出 0 就直接断言自杀（D2R 的 sdk 读 6/7，Unity 的 sdk 读 21/22，缺的那格默认给 0）。
        // DM_INFOLOG=1: report every GetInfo as "which item was asked, what was answered". Turn it on for a new title's first round:
        // the guest computes its default heap and memory pools from these, and a zero makes it assert straight away (D2R's sdk reads 6/7,
        // Unity's reads 21/22, and any item not handled falls through the default above as 0).
        { static int il = -1; static unsigned nic;
          if (il < 0) il = getenv("DM_INFOLOG") ? 1 : 0;
          if (il && nic++ < 128) LOG("[info] GetInfo(%#llx) -> %#llx", (unsigned long long)id, (unsigned long long)v); }
        regs[0] = 0; regs[1] = v; return 0;
    }
    case 0x1F: {   // ConnectToNamedPort(out_handle*, name)：假服务管理器（hle_ipc.c） | fake service manager
        void dm_ipc_connect(uint64_t *); dm_ipc_connect(regs); return 0;
    }
    case 0x21: {   // SendSyncRequest(会话句柄)：报文在这条线程的 TLR+0x000，按 CMIF 回「成功、无输出」 | the message sits at TLR+0x000
        void dm_ipc_send(uint64_t *); dm_ipc_send(regs); return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1;
            module_t *cm = module_of((void *)regs[30]), *sm_ = module_of((void *)orig_addr);
            char nmbuf[64] = {0}; const char *pnm = "?";
            if ((num == 0x1f || num == 0x20 || num == 0x21 || num == 0x29) && regs[1] > 0x10000) {
                const char *q = (const char *)(uintptr_t)regs[1];
                size_t qn = 0; while (qn < 31 && q[qn] >= 0x20 && q[qn] < 0x7f) { nmbuf[qn] = q[qn]; qn++; }
                if (qn >= 2) pnm = nmbuf;
            }
            LOG("SVC %#x (占位返回成功) x0=%#llx x1=%#llx x2=%#llx  站点=%s+%#llx  调用者=%s+%#llx  名字=%s", num, regs[0], regs[1], regs[2],
                sm_ ? sm_->name : "?", (unsigned long long)(sm_ ? orig_addr - (uint64_t)sm_->base : orig_addr),
                cm ? cm->name : "?", (unsigned long long)(cm ? (uint64_t)regs[30] - (uint64_t)cm->base : regs[30]), pnm); }
        regs[0] = 0; return 0;
    }
}
