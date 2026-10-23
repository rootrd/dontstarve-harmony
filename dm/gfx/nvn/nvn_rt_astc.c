/* 设备端 BC3 → ASTC 4x4 转码（B 路）。
 *
 * 为什么要它：马良 920 一个 BC 块格式都不认（第 81 轮实测），只有 ASTC 4x4 可采样。原来的两条路都有代价 ——
 * CPU 软解成 RGBA8 要 4× 显存；离线把素材烘成 ASTC 又必须先把素材清单数全（10-19 那次就是漏了 zip 里的
 * 3883 张，把整个实体层读成垃圾）。这条第三路让设备自己在上传那一刻转：BC3 块 → RGBA8 → ASTC 块，
 * 贴图按 ASTC 建。它不看文件名、不看清单，每张图在唯一一次真上传时自己表态，所以"漏了哪个容器"这类
 * 事故在结构上不可能发生。代价是编码耗时，全部记在 rt_astc_stat 里，供与离线烘对比取参数。
 *
 * 编码器是 Arm 官方参考实现 astcenc 5.7.0，构建机交叉编成 aarch64-linux-ohos 静态库
 * （third_party/astcenc/ohos/），libc++ 静态链进 .so，HAP 不多带一个 .so。
 * 编完的结果按内容寻址落盘（DM_RT_ASTC_DIR，见下面的缓存段），所以第二次启动不再重烧编码 CPU。
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "astcenc.h"
#include "nvn_rt_astc.h"
#include "../tex/nvn_bcdec.h"   // BC3 → RGBA8 那半截复用宿主一直在用的解码器（软解那条路同一个实现）

static struct astcenc_context *g_enc;
static float g_preset = -1.0f;
static char g_err[160];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t g_tex, g_blocks, g_ns, g_fail;
// 落盘缓存的账：命中（读盘没编）、新写、盘上出过错、额度用尽后跳过的写
static uint64_t g_hit, g_wrote, g_io, g_capped, g_bytes;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const char *rt_astc_err(void) { return g_err; }

int rt_astc_init(float preset)
{
    if (preset <= 0.0f) preset = 10.0f;   /* 没给档位就按 FAST：设备自转先要保证不卡 */
    pthread_mutex_lock(&g_mu);
    if (g_enc && preset == g_preset) { pthread_mutex_unlock(&g_mu); return 0; }
    if (g_enc) { astcenc_context_free(g_enc); g_enc = NULL; }
    struct astcenc_config cfg;
    enum astcenc_error e = astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, preset, 0, &cfg);
    if (e != ASTCENC_SUCCESS) {
        snprintf(g_err, sizeof g_err, "config 失败：%s", astcenc_get_error_string(e));
        pthread_mutex_unlock(&g_mu);
        return 1;
    }
    e = astcenc_context_alloc(&cfg, 1, &g_enc, NULL);
    if (e != ASTCENC_SUCCESS) {
        snprintf(g_err, sizeof g_err, "上下文失败：%s", astcenc_get_error_string(e));
        g_enc = NULL;
        pthread_mutex_unlock(&g_mu);
        return 2;
    }
    g_preset = preset;
    g_err[0] = 0;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int rt_astc_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out, size_t out_cap)
{
    if (!g_enc || !w || !h || !rgba || !out) return 1;
    uint32_t nbx = (w + 3u) / 4u, nby = (h + 3u) / 4u;
    size_t need = (size_t)nbx * (size_t)nby * 16u;
    if (out_cap < need) return 2;
    const void *slice = rgba;
    struct astcenc_image img = {
        .dim_x = w, .dim_y = h, .dim_z = 1, .data_type = ASTCENC_TYPE_U8,
        .data = (void **)&slice };
    const struct astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
    uint64_t t0 = now_ns();
    enum astcenc_error e = astcenc_compress_image(g_enc, &img, &swz, out, need, 0);
    uint64_t dt = now_ns() - t0;
    if (e != ASTCENC_SUCCESS) {
        snprintf(g_err, sizeof g_err, "编码失败：%s", astcenc_get_error_string(e));
        rt_astc_count(0, dt, 1);
        return 3;
    }
    rt_astc_count((uint64_t)nbx * nby, dt, 0);
    return 0;
}

/* BC3 块流 → ASTC 块：中间那块 RGBA8 是私有暂存，只在重放线程里用（两条上传路都在这个线程），所以不加锁。
   行距必须按 astcenc 的口径给"宽×4"紧排，不能像 Vulkan 那样 16 对齐。 */
static uint8_t *g_rgba;
static size_t g_rgba_cap;

/* ---------------- 落盘缓存 ----------------
 * 为什么要它：全库约 1.28 亿个 4×4 块，第 121 轮实测设备编到 163 k 块/秒 ⇒ 十几分钟的编码 CPU，每次启动都重烧一遍。
 * 同一份原厂 BC3 字节在同一档位下编出来的 ASTC 块必然逐字节相同（astcenc 没有随机/时间成分），所以按内容寻址存盘，
 * 第二次启动就是读盘 —— 这正是"设备自己转的也能留下来、加载更快"那条。
 * 键取解排布之后的 BC3 块流（两条上传路喂进这个函数的都是它）+ 宽高 + 档位：换档不会读到上一档的产物，
 * 素材换了字节也不会认错图。目录由 DM_RT_ASTC_DIR 给绝对路径，和 DM_TEXDUMP 同一口径，沙箱里可见才收得走。
 * 写用"临时名 + rename"：进程被杀在写中间不会留下半个文件被下次当成命中；读回还要卡死精确尺寸（多一字节也不算命中）。
 */
static char g_cdir[448];
static char g_cwhy[96];
static int g_cstate;          // 0 = 还没试，1 = 可用，-1 = 不可用（原因见 g_cwhy）
static uint64_t g_climit;     // 缓存字节额度，防止把手机存储写满

static uint64_t fnv1a(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static uint64_t rt_key(const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, float preset)
{
    uint32_t nbx = (w + 3u) / 4u, nby = (h + 3u) / 4u;
    uint64_t k = 1469598103934665603ull;
    for (uint32_t y = 0; y < nby; y++) k = fnv1a(src + (size_t)y * src_pitch, (size_t)nbx * 16u, k);
    k = fnv1a((const uint8_t *)&w, sizeof w, k);
    k = fnv1a((const uint8_t *)&h, sizeof h, k);
    int32_t pt = (int32_t)(preset * 10.0f);
    return fnv1a((const uint8_t *)&pt, sizeof pt, k);
}

static int mk_dirs(const char *path)
{
    char buf[448];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(buf, 0755) && errno != EEXIST) return -1;
        *s = '/';
    }
    if (mkdir(buf, 0755) && errno != EEXIST) return -1;
    return 0;
}

static const char *cache_dir(void)
{
    if (g_cstate) return g_cstate == 1 ? g_cdir : NULL;
    const char *d = getenv("DM_RT_ASTC_DIR");
    const char *c = getenv("DM_RT_ASTC_MAX");   // 额度（MB），默认 3072
    g_climit = c ? strtoull(c, NULL, 0) * (1024ull * 1024ull) : (3072ull << 20);
    if (!d || !d[0]) snprintf(g_cwhy, sizeof g_cwhy, "没给 DM_RT_ASTC_DIR");
    else if (d[0] != '/') snprintf(g_cwhy, sizeof g_cwhy, "要绝对目录，现在是 %s", d);
    else if (strlen(d) >= sizeof g_cdir) snprintf(g_cwhy, sizeof g_cwhy, "目录太长");
    else {
        snprintf(g_cdir, sizeof g_cdir, "%s", d);
        if (mk_dirs(g_cdir)) { snprintf(g_cwhy, sizeof g_cwhy, "建目录 errno=%d", errno); g_cdir[0] = 0; }
        else g_cstate = 1;
    }
    if (g_cstate != 1) g_cdir[0] = 0;
    return g_cstate == 1 ? g_cdir : NULL;
}

static void cache_bump(uint64_t *which, uint64_t add)
{
    pthread_mutex_lock(&g_mu);
    *which += add;
    pthread_mutex_unlock(&g_mu);
}

static int cache_get(uint64_t key, uint32_t w, uint32_t h, uint8_t *out, size_t need)
{
    const char *dir = cache_dir();
    if (!dir) return 0;
    char p[576];
    snprintf(p, sizeof p, "%s/a%016llx_%ux%u.astc", dir, (unsigned long long)key, w, h);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t rd = fread(out, 1, need, f);
    int over = (rd == need && fgetc(f) != EOF);   // 尺寸对不上就不是这张的产物，宁可重编
    fclose(f);
    if (rd != need || over) { cache_bump(&g_io, 1); return 0; }
    cache_bump(&g_hit, 1);
    return 1;
}

static void cache_put(uint64_t key, uint32_t w, uint32_t h, const uint8_t *out, size_t need)
{
    const char *dir = cache_dir();
    if (!dir) return;
    if (g_bytes >= g_climit) { cache_bump(&g_capped, 1); return; }
    char p[576], t[608];
    snprintf(p, sizeof p, "%s/a%016llx_%ux%u.astc", dir, (unsigned long long)key, w, h);
    snprintf(t, sizeof t, "%s/.w%016llx_%d.astc", dir, (unsigned long long)key, (int)getpid());
    FILE *f = fopen(t, "wb");
    if (!f || fwrite(out, 1, need, f) != need || fflush(f) || fsync(fileno(f))) {
        if (f) fclose(f);
        remove(t);
        cache_bump(&g_io, 1);
        return;
    }
    fclose(f);
    if (rename(t, p)) {
        remove(t);
        cache_bump(&g_io, 1);
        return;
    }
    cache_bump(&g_wrote, 1);
    pthread_mutex_lock(&g_mu); g_bytes += need; pthread_mutex_unlock(&g_mu);
}

int rt_bc3_to_astc(const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, uint8_t *out, size_t out_cap)
{
    if (!src || !out || !w || !h) return 1;
    size_t need = (size_t)((w + 3u) / 4u) * (size_t)((h + 3u) / 4u) * 16u;
    if (out_cap < need) return 2;
    uint64_t key = rt_key(src, src_pitch, w, h, g_preset);
    if (cache_get(key, w, h, out, need)) return 0;
    size_t rgba_cap = (size_t)w * (size_t)h * 4u;
    if (rgba_cap > g_rgba_cap) {
        uint8_t *p = realloc(g_rgba, rgba_cap);
        if (!p) return 4;
        g_rgba = p;
        g_rgba_cap = rgba_cap;
    }
    if (nvn_bc_decode(NVN_BC3, src, src_pitch, w, h, g_rgba, (size_t)w * 4u)) return 5;
    int rc = rt_astc_encode(g_rgba, w, h, out, out_cap);
    if (!rc) cache_put(key, w, h, out, need);
    return rc;
}

void rt_astc_count(uint64_t blocks, uint64_t ns, int fail){
    pthread_mutex_lock(&g_mu);
    if (fail) g_fail++;
    else { g_tex++; g_blocks += blocks; }
    g_ns += ns;
    pthread_mutex_unlock(&g_mu);
}

void rt_astc_stat(uint64_t *tex, uint64_t *blocks, uint64_t *ms, uint64_t *fail)
{
    pthread_mutex_lock(&g_mu);
    if (tex) *tex = g_tex;
    if (blocks) *blocks = g_blocks;
    if (ms) *ms = g_ns / 1000000ull;
    if (fail) *fail = g_fail;
    pthread_mutex_unlock(&g_mu);
}

void rt_astc_cache_stat(uint64_t *hit, uint64_t *wrote, uint64_t *io, uint64_t *capped, uint64_t *mb)
{
    pthread_mutex_lock(&g_mu);
    if (hit) *hit = g_hit;
    if (wrote) *wrote = g_wrote;
    if (io) *io = g_io;
    if (capped) *capped = g_capped;
    if (mb) *mb = g_bytes / (1024ull * 1024ull);
    pthread_mutex_unlock(&g_mu);
}

/* 取参数的那一行要知道缓存到底装没装上：目录开着回路径，关着回原因。两者都是同一轮跑出来的自证。 */
const char *rt_astc_cache_dir(void)
{
    cache_dir();
    return g_cstate == 1 ? g_cdir : NULL;
}

const char *rt_astc_cache_off(void) { return g_cwhy; }
