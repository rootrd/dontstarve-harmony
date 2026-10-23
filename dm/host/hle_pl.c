// nn::pl 共享字体：客体（Unity 的字体回退层）按名字表要系统字体，拿到「加载状态 / 地址 / 大小」三件后直接把那段内存交给它自己的排版器。
// v17 实测：main 只引用这三个入口（GetSharedFontLoadState / GetSharedFontAddress / GetSharedFontSize），名字表里有
// Switch_Font_STD / Switch_Font_CN / Switch_Font_KR / Switch_Font_NIN_Extended；它先比 `GetSharedFontLoadState(...)==1`，
// 不等就报错走别的路。走 SDK 客户端要经 pl:u 服务（'SFCI' 域对象），宿主那条通用 cmif 应答在 v17 里让客体
// 在 nn::pl 客户端里 R_ASSERT → Break Result=0x1a80a → 进程退出 42。所以这三个入口由宿主直接接管：状态一律报「已加载」，
// 地址给一块宿主读进来的一块真字体（同一块复给所有类型），大小给它的字节数。
// nn::pl shared fonts: the guest (Unity's font fallback layer) asks for the system fonts by name and, after getting
// {load state, address, size}, hands that memory straight to its own text engine. Measured in v17: main references exactly
// three entry points (GetSharedFontLoadState / GetSharedFontAddress / GetSharedFontSize) and its name table contains
// Switch_Font_STD / Switch_Font_CN / Switch_Font_KR / Switch_Font_NIN_Extended; it first tests `GetSharedFontLoadState(...)==1`.
// The SDK client would go to the pl:u service (an 'SFCI' cmif domain object); our generic cmif reply made the guest
// R_ASSERT inside the nn::pl client in v17 (Break Result=0x1a80a, process exit 42). So the host takes these three over:
// the state always says "loaded", the address is a real font the host read in (the same block for every type), the size is its length.
#include "hle.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdatomic.h>

// 字体来源：DM_PL_FONT=<绝对路径> 优先，否则按这张表找第一块读得动的。全是本机原厂系统字体（中文由 SC/HYQiHei 覆盖）。
// Font source: DM_PL_FONT=<absolute path> wins, otherwise the first readable entry of this list. All of them are the device's own system fonts.
static const char *g_font_try[] = {
    "/system/fonts/HarmonyOS_Sans_SC.ttf",
    "/system/fonts/HYQiHeiL3.ttf",
    "/system/fonts/HarmonyOS_Sans.ttf",
    "/system/fonts/NotoNaskhArabicUI-Regular.ttf",
};

static pthread_mutex_t g_pl_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_buf; static size_t g_len; static const char *g_src;

static void font_load_locked(void)
{
    if (g_buf) return;
    const char *cand[6]; int nc = 0;
    const char *env = getenv("DM_PL_FONT");
    if (env && *env) cand[nc++] = env;
    for (unsigned i = 0; i < sizeof g_font_try / sizeof *g_font_try && nc < 6; i++) cand[nc++] = g_font_try[i];
    for (int i = 0; i < nc; i++) {
        struct stat st; int fd = open(cand[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 4096) { if (fd >= 0) close(fd); continue; }
        size_t n = (size_t)st.st_size;
        uint8_t *b = malloc(n);
        if (!b) { close(fd); continue; }
        size_t got = 0;
        while (got < n) { ssize_t r = read(fd, b + got, n - got); if (r <= 0) break; got += (size_t)r; }
        close(fd);
        if (got != n) { free(b); continue; }
        g_buf = b; g_len = n; g_src = cand[i];
        LOG("pl 共享字体：%s（%zu 字节，魔数 %#02x%#02x%#02x%#02x）", g_src, g_len, b[0], b[1], b[2], b[3]);
        return;
    }
    LOG("【红】pl 共享字体：DM_PL_FONT 与系统字体目录都读不到（试了 %d 个），字体入口回空", nc);
    g_buf = NULL; g_len = 0; g_src = "(无)";
}

// 每个类型只报一次，防止每帧刷屏（游戏每帧都可能重取一次当前字体）
// Report each type once so the per-frame re-fetch cannot flood the log.
static void font_said(int type, const void *addr, size_t n)
{
    static _Atomic unsigned seen;
    unsigned mask = atomic_load(&seen);
    if (type < 0 || type >= 31) type = 0;
    if (mask & (1u << type)) return;
    atomic_fetch_or(&seen, 1u << type);
    LOG("pl::GetSharedFont 类型 %d → %p + %#zx（来源 %s）", type, addr, n, g_src);
}

HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",
          "共享字体加载状态：宿主带一块真字体，一律报 1（客体 main+0x57ab5c 判的就是 ==1）", int, h_pl_LoadState, int type) {
    (void)type; return 1;
}
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",
          "共享字体地址：给宿主读进来的那块字体开头", const void *, h_pl_Address, int type) {
    pthread_mutex_lock(&g_pl_lock); font_load_locked(); const void *p = g_buf; size_t n = g_len; pthread_mutex_unlock(&g_pl_lock);
    font_said(type, p, n);
    return p;
}
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",
          "共享字体大小：给那块字体的字节数", uint64_t, h_pl_Size, int type) {
    (void)type;
    pthread_mutex_lock(&g_pl_lock); font_load_locked(); uint64_t n = (uint64_t)g_len; pthread_mutex_unlock(&g_pl_lock);
    return n;
}
