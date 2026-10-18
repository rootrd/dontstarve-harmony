// 客体线程环境：门槽 = 宿主门入口；x18 槽清零；tpidr_el0 槽 = ELF TLS 块；tpidrro_el0 槽 = Horizon ThreadLocalRegion（0x200）。
// SDK 的内部临界区从 TLR[0x1f8] 取当前线程对象指针，这里给每线程分配一个零初始化对象并写入。
// Guest thread environment: gate slot = host gate entry; x18 slot cleared; tpidr_el0 slot = ELF TLS block; tpidrro_el0 slot = Horizon ThreadLocalRegion (0x200).
// The SDK's internal critical section takes the current thread object pointer from TLR[0x1f8]; here each thread gets a zero-initialized object allocated and written in.
#include "dm.h"
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
#define TLS_MOD_SIZE (64u << 10)   // 每模块 TLS 块（零初始化；初值由客体自己的 _ZTH/__tls_init 填）| per-module TLS block
extern void dm_gate(void);            // entry.S：门公共入口 | entry.S: common gate entry
#include "front.h"
#include "dm_uiscreen.h"
void guest_thread_init(void) {
    pthread_setspecific(g_keygate, (void *)dm_gate);
    for (int i = 0; i < 2; i++) if (g_keyres[i]) pthread_setspecific(g_keyres[i], (void *)dm_game_res(i));   // 分辨率参数位：{u32 宽, u32 高} 打包 | resolution parameter slot: {u32 width, u32 height} packed
    if (g_keyaspect || g_keyaspectui) {   // 宽高比上限参数位：max(19/9, 两种模式输出宽 / 高)，float 位模式放低 32 位 | aspect ratio limit parameter slot: max(19/9, output width / height of the two modes), float bit pattern in the low 32 bits
        float a = 19.0f / 9.0f; int wide = 0;
        for (int i = 0; i < 2; i++) { uint64_t r = front_game_res(i); float w = (float)(uint32_t)r, h = (float)(uint32_t)(r >> 32); if (h > 0 && w / h > a) { a = w / h; wide = 1; } }
        // 界面边柱着色器的上限：屏幕宽于 19:9 时另加 0.15，把边柱向内的柔边推出屏幕；16:9 不变。
        // 推导：边柱内缘 x = 屏高 × 上限 / 2，上限每加 Δ，内缘每侧外移 屏高 × Δ / 2。Mac 1952x900 实测柔边宽度 ≤ 30 像素
        // （上限 = 屏幕比例时右缘第 0–20 列由黑渐亮，上限推到 2.4 即消失）⇒ 需 Δ ≥ 2 × 30 / 900 ≈ 0.067；取 0.15（每侧外移约 67 像素）留出一倍以上余量。
        // 不是魔数：屏高变大时余量按比例不变（Δ 是相对屏高的量）
        // Limit of the UI side-pillar shader: when the screen is wider than 19:9 add another 0.15, pushing the pillars' inward soft edge off the screen; 16:9 is unchanged.
        // Derivation: pillar inner edge x = screen height * limit / 2, so for every delta added to the limit the inner edge moves outward by screen height * delta / 2 on each side. Measured on Mac 1952x900, the soft edge is <= 30 pixels wide
        // (with limit = screen aspect ratio, columns 0–20 at the right edge fade from black to bright, and it disappears once the limit is pushed to 2.4) => need delta >= 2 * 30 / 900 ~ 0.067; 0.15 is taken (about 67 pixels outward per side), leaving more than double the margin.
        // Not a magic number: when the screen height grows the margin stays proportional (delta is relative to screen height)
        float ui = wide ? a + 0.15f : a;
        uint32_t bits;
        if (g_keyaspect) { memcpy(&bits, &a, 4); pthread_setspecific(g_keyaspect, (void *)(uintptr_t)bits); }
        if (g_keyaspectui) { memcpy(&bits, &ui, 4); pthread_setspecific(g_keyaspectui, (void *)(uintptr_t)bits); }
    }
    pthread_setspecific(g_key18, 0);
    // Horizon TLS：客体代码用 mrs tpidr_el0 拿「线程本地区」，再从 +8 取「按模块索引的 TLS 块指针表」，
    // 然后 tls_table[module] + 偏移 访问自己的 thread_local 变量（C++ 的 _ZTH 初始化函数也走这条路）。
    // 所以 tpidr_el0 与 tpidrro_el0 必须指向同一个区域，且 +8 要有表；原先 KTLS 给的是另一块零缓冲 ⇒
    // 客体读到 0 再按 0 索引（饥荒的 cApplicationBase 一上来就用 thread_local，撞的就是这里）。
    // Horizon TLS: guest code reads the thread-local region through `mrs tpidr_el0`, then takes the per-module TLS block
    // pointer table from +8 and accesses its thread_local variables as tls_table[module] + offset (the C++ _ZTH initializers
    // go the same way). So tpidr_el0 and tpidrro_el0 must point at the same region and +8 must hold that table; the old code
    // gave KTLS a different zeroed buffer, so the guest read 0 and indexed from 0.
    uint8_t *tlr = calloc(1, TLR_SIZE);
    *(void **)(tlr + 0x1f8) = calloc(1, THREAD_OBJ_SIZE);   // 当前线程对象 | current thread object
    extern int g_nmod;
    {
        unsigned n = (unsigned)(g_nmod > 0 ? g_nmod : 4) + 4;   // 多给几槽余量 | a few spare slots
        void **tab = calloc(n, sizeof *tab);
        for (unsigned i = 0; i < n; i++) tab[i] = calloc(1, TLS_MOD_SIZE);
        *(void **)(tlr + 8) = tab;
        LOG("客体 TLS：区域 %p，模块表 %p（%u 槽 × %u 字节，模块数 %d）", (void *)tlr, (void *)tab, n, TLS_MOD_SIZE, g_nmod);
    }
    pthread_setspecific(g_keytls, tlr);
    pthread_setspecific(g_keytlr, tlr);
}
