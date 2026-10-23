// P1+P2 验收：把生产文件里的暂存环（stage_begin/stage_take/乘加判据）、退避、队列票据、冲刷上限、
// pkg_upload/pkg_queue/pkg_flush/tex_new_life、统计映射表和复活/延迟销毁那几段原样抽出来，
// 在这台宿主机上按「故障注入」逐条断言——被测的就是装机那一份代码，桩只负责让某一格失败。
// The production bodies are awk-extracted verbatim (the staging ring included); only the helpers are stubs, so a
// refusal at headroom / capacity / allocation, a wrapped footprint, a stale ticket or a fault at mip 1 / layer 1 is
// exercised against the real state machine, not a hand-written model of it.
// 跑法：scripts/pkg_faults.sh（它负责抽段、编、判）；加 NEG=<ref> 抽旧版，同一套断言必须判红。
// Run it with scripts/pkg_faults.sh; NEG=<git-ref> re-runs the same assertions against an older version, which must go red.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <vulkan/vulkan.h>

#define GVL(...) printf(__VA_ARGS__)
#define ARENA_SZ (64ull << 20)      // 只为 bigs_build 那句就绪打印：环大小按源码里的 BIG_STAGE_SZ 实打实分配
#define GRED_MAX 200
static int g_red_once;
#define GRED(...) do { if (g_red_once < GRED_MAX) { g_red_once++; GVL("[red] " __VA_ARGS__); } } while (0)
enum { NVN_TRACE_QUEUE, NVN_TRACE_ATTEMPT, NVN_TRACE_RESULT, NVN_TRACE_DROP };

// ---- 生产代码要的类型（与 nvn_tex.h / nvn_obj.h / nvn_deswizzle.h 逐字段对齐）| mirrors of the real types ----
// pkg_stage 不在这里：它和整段暂存环都从源码抽，判的就是那几行减法。
typedef struct { uint64_t size; } pool_t;
typedef struct { int target, format, flags, samples; uint32_t width, height, depth, levels, stride; int pkg_block_h; } nvn_texdesc;
typedef struct { uint64_t size, layer_size; uint32_t align, layers, bpb; unsigned tileH, tileD; } nvn_texlayout;
typedef struct { uint32_t magic; nvn_texdesc d; nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off;
                 uint8_t has_layout, layout[8]; uint64_t serial; } tex_t;

// ---- 生产全局（vktex / 计数器 / 队列 / 暂存环在下面的 body.inc 里，其余在这里补齐）----
static VkCommandBuffer g_cb; static int g_in_pass, g_sets_valid; static int g_skip;
static VkDevice g_dev;
static uint64_t g_presents, g_tex_stale, g_tex_gen, g_tex_revive, g_fence_val;
static _Atomic uint64_t g_sub_reenter, g_sub_race;   // 这两枚在源码文件头部，不在抽取范围内

#ifdef OLD
// 旧版没有分档与退避这一套。补上同名全局（永远没人写），同一句断言在旧版必然读到 0 = 判红；
// 这样 NEG 跑用的是同一份断言，不是另写一套「专供旧版」的宽松版。
enum { STG_OK = 0, STG_HEAD = 1, STG_OVER = 2, STG_NOBUF = 3 };
static _Atomic uint64_t g_pkg_ovfl, g_pkg_head, g_pkg_over, g_pkg_nobuf, g_pkg_bkoff, g_pkg_age,
                        g_pkg_qfull, g_pkg_tk_moved;
#endif

// ---- 可编程桩 | the injectable stubs（vktex * 一律走 void *：桩在这些定义之前，见上面注释）----
static uint8_t *g_ring;              // mk_hostbuf 交出去的暂存环（源码按 BIG_STAGE_SZ 要 = 160 MB）
static int g_ring_fail;              // 1 = 建环失败（「无环」那一档）
static int n_mkbuf;                  // 分配试了几次：建环失败后不该每图再试一次
static int g_bw = 1, g_bh = 1, g_bpb = 4, g_bpp = 4, g_bcok = 1;
static uint32_t g_rowb = 1024, g_rows = 8, g_sl = 1;   // 每级每层的块行字节 / 块行数 / 切片数
static uint32_t g_rows_late = 0;     // 第二趟（备料）改口的块行数：造「量尺算少了」那一格
static int g_late_after = -1;        // 从第几次 geom 调用起改口
static int n_geom;
static int g_geom_fail_lvl = -1, g_geom_fail_layer = -1;
static int g_dz_fail_lvl = -1, g_dz_fail_layer = -1, g_dec_fail = 0;
static uint64_t g_base_step = 1024 * 8;   // 本级本层在池里的起点：造越界用
static int g_tracked = 1;
static _Atomic uint64_t g_eup_try, g_eup_ok, g_eup_cap, g_eup_fail;
static int n_protect, n_layout, n_readarm, n_enter, n_drop, n_result, n_queued;
static uint8_t g_poolbytes[16 << 20];

static void textrace(unsigned ev, const void *v, int tid, const char *det) {
    (void)v; (void)tid; (void)det;
    if (ev == NVN_TRACE_ATTEMPT) n_enter++; else if (ev == NVN_TRACE_DROP) n_drop++;
    else if (ev == NVN_TRACE_RESULT) n_result++; else if (ev == NVN_TRACE_QUEUE) n_queued++;
}
static void to_layout(VkCommandBuffer cb, void *v, VkImageLayout want) { (void)cb; (void)v; (void)want; n_layout++; }
static uint64_t nvn_dirty_now(void) { return ++g_tex_gen; }
static int nvn_dirty_tracked(const void *a, size_t n) { (void)a; (void)n; return g_tracked; }
static void nvn_dirty_protect(const void *a, size_t n) { (void)a; (void)n; n_protect++; }
static int nvn_fmt_bpb(int f, int *bw, int *bh) { (void)f; *bw = g_bw; *bh = g_bh; return g_bpb; }
static int nvn_bc_unpacked(uint32_t f, uint32_t *pf, int *bpp) {
    (void)f; if (!g_bcok) return 0; *pf = VK_FORMAT_R8G8B8A8_UNORM; *bpp = g_bpp; return 1;
}
static int mk_hostbuf(uint64_t size, uint32_t usage, VkBuffer *b, VkDeviceMemory *m, void **map) {
    (void)usage; n_mkbuf++;
    if (g_ring_fail) return 0;
    if (!g_ring) g_ring = malloc((size_t)size);   // 只此一块：环游标由生产自己管，桩不插手
    if (!g_ring) return 0;
    *b = (VkBuffer)(intptr_t)1; *m = (VkDeviceMemory)(intptr_t)2; *map = g_ring;
    return 1;
}
static int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned lvl, unsigned la, uint32_t *rb, uint32_t *rows,
                              uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    (void)d; (void)lvl; (void)la;
    n_geom++;
    if ((int)lvl == g_geom_fail_lvl || (int)la == g_geom_fail_layer) return 1;
    *rb = g_rowb;
    *rows = (g_rows_late && n_geom > g_late_after) ? g_rows_late : g_rows;
    *slices = g_sl;
    *base = (uint64_t)lvl * g_base_step + (uint64_t)la * (1u << 20);
    *th = *td = 0; *bpb = 4; return 0;
}
static uint64_t g_source_padding;
// This harness uses a linear fake storage layout; pixel tiling is tested by vk_compat.c.
static uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t rb, uint32_t rows, unsigned th, unsigned td) {
    (void)th; (void)td; return ((uint64_t)z * rows + y) * rb + x + g_source_padding;
}
static int nvn_deswizzle(const nvn_texdesc *d, unsigned lvl, unsigned la, const void *src, void *dst, size_t pitch) {
    (void)d; (void)src;
    if ((int)lvl == g_dz_fail_lvl || (int)la == g_dz_fail_layer) return -1;
    memset(dst, 0xA5, pitch); return 0;      // 只碰环里批给这一块的那点字节
}
static int nvn_bc_decode(uint32_t f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dp) {
    (void)f; (void)src; (void)pitch; (void)dp;
    if (g_dec_fail) return -1;
    if (dst) dst[0] = 0x5A;                  // 解码写出一个字节就够：这里判的是结局不是像素
    (void)w; (void)h; return 0;
}
static int rt_bc3_to_astc(const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t cap) {
    (void)src; (void)pitch; (void)w; (void)h;
    if (g_dec_fail || !cap) return -1;
    dst[0] = 0x5a;
    return 0;
}
// 取证三探针（A/B 落盘、整张落盘、读回挂账）：桩把「版本代号 / 层」记下来，C6 就判这两个身份
// 在两条探针里是不是同一个（P3 的「同一版本、同一层」全靠这两个字段，名字错了整轮证据就作废）。
// OLD 版还没有这些参数，桩跟着源码分两套：反例跑必须编得过，才能拿同一套断言判它红。
static int n_dump, n_raw;
static uint64_t d_stamp; static unsigned d_la, d_lvl;
static uint64_t r_stamp; static unsigned r_la;
#ifdef OLD
static void tex_dump(uint64_t s, unsigned l, unsigned n, const void *a, size_t an, const void *b, size_t bn,
                     uint32_t w, uint32_t h, uint32_t rb, int nf, int vf, int soft) {
    (void)n; (void)a; (void)an; (void)b; (void)bn; (void)w; (void)h; (void)rb; (void)nf; (void)vf; (void)soft;
    n_dump++; d_stamp = 0; d_la = 0; d_lvl = l; (void)s;
}
static void tex_dump_raw(const nvn_texdesc *d, uint64_t s, const void *m, size_t a) { (void)d; (void)s; (void)m; (void)a; n_raw++; }
static void tex_read_arm(VkCommandBuffer cb, void *v, unsigned lvl) { (void)cb; (void)v; (void)lvl; n_readarm++; r_stamp = 0; r_la = 0; }
#else
static unsigned g_dump_layer, g_read_layer;     // DM_TEXDUMP_LAYER / DM_TEXREAD_LAYER 的替身
static void p3_layers(unsigned *dl, unsigned *rl) { *dl = g_dump_layer; *rl = g_read_layer; }
static void tex_dump(uint64_t s, uint64_t stamp, unsigned l, unsigned la, unsigned n,
                     const void *a, size_t an, const void *b, size_t bn,
                     uint32_t w, uint32_t h, uint32_t rb, int nf, int vf, int soft) {
    (void)s; (void)n; (void)a; (void)an; (void)b; (void)bn; (void)w; (void)h; (void)rb; (void)nf; (void)vf; (void)soft;
    n_dump++; d_stamp = stamp; d_la = la; d_lvl = l;
}
static void tex_dump_raw(const nvn_texdesc *d, uint64_t s, uint64_t stamp, const void *m, size_t a) {
    (void)d; (void)s; (void)stamp; (void)m; (void)a; n_raw++;
}
static void tex_read_arm(VkCommandBuffer cb, void *v, unsigned lvl, unsigned la, uint64_t stamp) {
    (void)cb; (void)v; (void)lvl; n_readarm++; r_stamp = stamp; r_la = la;
}
#endif
static void up_probe(const char *t, const uint8_t *p, uint64_t n, uint32_t w, uint32_t h, int tally) {
    (void)t; (void)p; (void)n; (void)w; (void)h; (void)tally;
}
// 签名跟着源码走：新版多带一个「原因」，旧版三参数
#ifdef OLD
static void pkg_big_said(const void *v, uint64_t tot, uint64_t cmax) { (void)v; (void)tot; (void)cmax; }
#else
static void pkg_big_said(const void *v, uint64_t tot, uint64_t cmax, int why) { (void)v; (void)tot; (void)cmax; (void)why; }
#endif

struct rec { uint64_t off; uint32_t lvl, layer, cnt, ew, eh, ed, brl, bih; };
static struct rec cop[1024]; static int ncopy;
void vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout lay,
                            uint32_t n, const VkBufferImageCopy *r) {
    (void)cb; (void)src; (void)dst; (void)lay;
    for (uint32_t i = 0; i < n && ncopy < 1024; i++)
        cop[ncopy++] = (struct rec){ r[i].bufferOffset, r[i].imageSubresource.mipLevel,
            r[i].imageSubresource.baseArrayLayer, r[i].imageSubresource.layerCount,
            r[i].imageExtent.width, r[i].imageExtent.height, r[i].imageExtent.depth,
            r[i].bufferRowLength, r[i].bufferImageHeight };
}

// 坟场用的销毁桩：只数调了几次，不碰真句柄（这里判的是时机不是 Vulkan）
static int n_dview, n_dimg, n_dmem;
void vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; n_dview++; }
void vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)i; (void)a; n_dimg++; }
void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; n_dmem++; }

#include "body.inc"          // vktex / 计数器 / 队列 / 暂存环 / 退避 / 统计映射 / 四个主函数：全部来自 nvn_vulkan.c
#include "grave.inc"         // 延迟销毁那一整段（HEAD 旧版没有，NEG 跑时是空文件）

// ---- 读生产全局的门面（旧版没这些字段/表，一律回一个「对不上」的值，让断言判红而不是编译不过）----
#define LD(c) atomic_load(&(c))
static uint64_t v_next(const vktex *v) {
#ifdef OLD
    (void)v; return ~(uint64_t)0;
#else
    return v->pkg_next;
#endif
}
static int v_why(const vktex *v) {
#ifdef OLD
    (void)v; return STG_NOBUF;   // 旧版不分档：任何「原因等于某一档」的断言都判不到
#else
    return v->pkg_why;
#endif
}

// 排队一律走生产的 pkg_queue（票据字段由它自己填）；桩不代排队，否则测的就不是那个状态机。

// ---- 断言骨架 | the assertion spine ----
static int fails, checks;
static void ck(const char *what, long long got, long long want) {
    checks++;
    if (got != want) { fails++; printf("【红】%s 得 %lld 期 %lld\n", what, got, want); }
}
static void casebeg(const char *name) { printf("\n--- %s\n", name); }

// 跨用例只增不减的都是生产计数：一律先拍快照再算差值
struct snap { uint64_t big, bigok, arena, head, over, nobuf, bkoff, age, ovfl, moved, qfull, up, re; };
static struct snap snap_get(void) {
    return (struct snap){ LD(g_pkg_big), LD(g_pkg_bigok), LD(g_pkg_arena), LD(g_pkg_head), LD(g_pkg_over),
                          LD(g_pkg_nobuf), LD(g_pkg_bkoff), LD(g_pkg_age), LD(g_pkg_ovfl), LD(g_pkg_tk_moved),
                          LD(g_pkg_qfull), LD(g_pkg_up), LD(g_pkg_re) };
}
#define D(s, f) ((long long)(snap_get().f - (s).f))

static pool_t g_pool; static tex_t g_td; static vktex g_v, g_v2;
static vktex g_many[300];
static void mk(vktex *v, uint64_t ser, uint64_t img, uint32_t w, uint32_t h, uint32_t lv, uint32_t la, int soft) {
    memset(v, 0, sizeof *v);
    memset(&g_td, 0, sizeof g_td);
    g_td.serial = ser; g_td.d.format = soft ? 0x42 : 0x25; g_td.d.width = w; g_td.d.height = h;
    g_td.d.levels = lv; g_td.d.depth = 1; g_td.d.target = 1;
    g_td.L.layers = la;
    g_td.mem = g_poolbytes; g_pool.size = sizeof g_poolbytes; g_td.pool = &g_pool; g_td.off = 0;
    v->serial = ser; v->d = g_td; v->img = (VkImage)(uintptr_t)img; v->view = (VkImageView)(uintptr_t)img;
    v->w = w; v->h = h; v->levels = lv; v->layers = la; v->soft_bc = soft; v->fmt = VK_FORMAT_R8G8B8A8_UNORM;
}
static void clearday(void) {
    memset(g_pkg_pend, 0, sizeof g_pkg_pend); g_pkg_pend_n = 0;
    ncopy = n_enter = n_drop = n_result = n_queued = n_layout = n_readarm = n_protect = n_mkbuf = 0;
    n_dump = n_raw = 0; d_stamp = r_stamp = 0; d_la = d_lvl = r_la = 0;
#ifndef OLD
    g_dump_layer = g_read_layer = 0;   // 探针默认第 0 层：上一条指到别层的，到这儿得擦回去
#endif
    g_geom_fail_lvl = g_dz_fail_lvl = g_geom_fail_layer = g_dz_fail_layer = -1; g_dec_fail = 0;
    g_bcok = 1; g_tracked = 1; g_skip = 0;
    g_rowb = 1024; g_rows = 8; g_sl = 1; g_base_step = 1024 * 8; g_bpb = 4; g_bw = g_bh = 1;
    g_rows_late = 0; g_late_after = -1; n_geom = 0;
    g_ring_fail = 0; g_source_padding = 0;
    // 环本身在源码里是「建一次就长住」的：用例之间得把这三个句柄擦回去，否则 g_ring_fail 那一档永远走不到
    g_bigs = (VkBuffer)0; g_bigs_mem = (VkDeviceMemory)0; g_bigs_map = NULL;
    mtl_sub_reset_bigstage();          // 相当于换了一条提交：环游标清零（「余量不足等清零就有救」的那个清零）
    atomic_store(&g_birs_bad, 0);      // 建环失败源码里只认一次，用例之间得把它擦干净
    n_dview = n_dimg = n_dmem = 0;
}

#ifndef OLD
// 手抄一张「标签 ↔ 计数器」的期望表，和生产那张对着干：生产把标签配错计数器、或漏了一条，
// 这里就报红。第 40 轮那次「标签对上隔壁的数」就是靠位置蒙过去的。
static const struct { const char *lab; _Atomic uint64_t *c; int shift; } g_exp[] = {
    { "尝试图", &g_pkg_attempted_textures, 0 }, { "上", &g_pkg_up, 0 }, { "重", &g_pkg_re, 0 },
    { "成MB", &g_pkg_bytes, 20 }, { "BC层", &g_pkg_bc, 0 }, { "卡环", &g_pkg_arena, 0 },
    { "越界", &g_pkg_range, 0 }, { "解排", &g_pkg_dz, 0 }, { "格式", &g_pkg_fmt, 0 },
    { "深度", &g_pkg_dep, 0 }, { "三维", &g_pkg_3d, 0 }, { "环拒", &g_pkg_big, 0 },
    { "环成", &g_pkg_bigok, 0 }, { "队满", &g_pkg_qfull, 0 }, { "换位", &g_pkg_tk_moved, 0 },
    { "超表", &g_pkg_sub_over, 0 }, { "溢出", &g_pkg_ovfl, 0 }, { "余不", &g_pkg_head, 0 },
    { "超容", &g_pkg_over, 0 }, { "无环", &g_pkg_nobuf, 0 }, { "退避", &g_pkg_bkoff, 0 },
    { "队龄", &g_pkg_age, 0 }, { "重开", &g_sub_reenter, 0 }, { "串线", &g_sub_race, 0 },
    { "当场试", &g_eup_try, 0 }, { "当场成", &g_eup_ok, 0 },
    { "当场顶", &g_eup_cap, 0 }, { "当场败", &g_eup_fail, 0 },
};
#define EXP_N ((int)(sizeof g_exp / sizeof g_exp[0]))

// 把「｜打包:尝试图1000」这类 token 拆成（期望表下标, 数值）；靠「标签是 token 去掉尾数的后缀」认，
// 所以表里换了顺序也不会误判，只有配错计数器才会红。
static int tok_which(const char *tok, long *num) {
    int j = 0; while (tok[j]) j++;
    int k = j; while (k && tok[k - 1] >= '0' && tok[k - 1] <= '9') k--;
    if (k == j || k > 63) return -1;
    char lead[64]; memcpy(lead, tok, (size_t)k); lead[k] = 0;
    *num = strtol(tok + k, NULL, 10);
    for (int i = 0; i < EXP_N; i++) {
        size_t ll = strlen(g_exp[i].lab), pl = strlen(lead);
        if (pl >= ll && !memcmp(lead + pl - ll, g_exp[i].lab, ll)) return i;
    }
    return -1;
}
#endif

int main(void) {
    g_cb = (VkCommandBuffer)(intptr_t)7;
    printf("== 故障注入：%s\n",
#ifdef OLD
           "HEAD 旧版（应有多条红）"
#else
           "工作区新版"
#endif
    );

    casebeg("Tegra padding exceeds pool even when linear bytes fit");
    clearday(); mk(&g_v, 10, 0x100, 64, 64, 1, 1, 0);
    g_pool.size = (uint64_t)g_rowb * g_rows; g_source_padding = 512;
    pkg_queue(&g_v); pkg_flush();
    ck("padded source rejected", g_v.pkg_bad, 1);
    ck("no out-of-range copy", ncopy, 0);

    // ① 陈旧标志：进队要成、传成才清（C3）
    casebeg("C3-1 stale=1/dirty=0/uploaded=1 必须进得了队，成功才清");
    clearday(); mk(&g_v, 11, 0x110, 128, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_stale = 1;
    pkg_queue(&g_v);
    ck("进队", g_pkg_pend_n, 1);
    pkg_flush();
    ck("冲刷后 stale 清零", g_v.pkg_stale, 0);
    ck("冲刷后仍算传过", g_v.uploaded, 1);
    ck("没判坏", g_v.pkg_bad, 0);
    ck("录了一次拷贝", ncopy, 1);

    // ② 已经是新的：pkg_queue 自己那道门还在（不能把「无需求」也排进去）
    casebeg("C3-2 uploaded 且不带脏/陈旧 = 直接拒（这道门要留着）");
    clearday(); mk(&g_v, 12, 0x120, 64, 64, 1, 1, 0); g_v.uploaded = 1;
    pkg_queue(&g_v);
    ck("被拒", g_pkg_pend_n, 0);

    // ③ 队满：请求留在资源身上，队一空就补得回来
    casebeg("C3-3 队满不算丢请求：清空队列后仍能进队并上传");
    clearday(); mk(&g_v, 13, 0x130, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    memset(g_pkg_pend, 0, sizeof g_pkg_pend); g_pkg_pend_n = PKG_PEND;
    pkg_queue(&g_v);
    ck("队满时没挤进去", g_pkg_pend_n, PKG_PEND);
    ck("队满后脏标志还在", g_v.pkg_dirty, 1);
    pkg_flush();                       // 队列里全是作废票据：一张也不动
    ck("作废队列没误传", n_enter, 0);
    ck("作废队列没吃掉这张的脏", g_v.pkg_dirty, 1);
    pkg_queue(&g_v);
    ck("腾出来后进队", g_pkg_pend_n, 1);
    pkg_flush();
    ck("这回传上了", g_v.pkg_dirty, 0);
    ck("拷贝录了一张", ncopy, 1);

    // ④ 本提交余量不足：连拒 9 次也不许判死，游标清零 + 退过窗口就补得回来（C3 尾 + P2）
    casebeg("C3-4 余量不足=可重试：拒 9 次不置坏，换条提交后传成、账目清零");
    clearday(); mk(&g_v, 14, 0x140, 256, 256, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    for (int k = 1; k <= 9; k++) {
        g_bigs_off = BIG_STAGE_SZ - 1024;              // 每条「提交」都只剩 1 KB，而这张量出来要 8 KB
        g_fence_val += 4096;                           // 拉开到退避窗口之外：这里数的是拒绝次数
        g_v.pkg_dirty = 1;
        g_pkg_pend_n = 0; pkg_queue(&g_v);             // 相当于这一帧又绑了一次：票据按新代号重排（队龄才不会把这张挤掉）
        pkg_flush();
        if (g_v.pkg_bad) { checks++; fails++; printf("【红】第 %d 次拒绝就判死了\n", k); break; }
        checks++;
    }
    ck("拒了 9 次", g_v.pkg_rej, 9);
    ck("坏标志还是 0", g_v.pkg_bad, 0);
    ck("请求还在（脏没清）", g_v.pkg_dirty, 1);
    ck("一条拷贝都没录", ncopy, 0);
    mtl_sub_reset_bigstage(); g_fence_val += 4096;     // 换提交：游标清零
    g_pkg_pend_n = 0; pkg_queue(&g_v);
    pkg_flush();
    ck("后来传成了", g_v.pkg_dirty, 0);
    ck("瞬时拒绝清零", g_v.pkg_rej, 0);

    // ⑤ 第 1 级几何错：量尺那一趟就拒，一条拷贝都不许录（C5）
    casebeg("C5-1 第 1 级几何失败 ⇒ 0 条拷贝、不判成功、请求不被吃掉");
    clearday(); mk(&g_v, 15, 0x150, 256, 128, 2, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_geom_fail_lvl = 1;
    pkg_queue(&g_v); pkg_flush();
    ck("没录拷贝", ncopy, 0);
    ck("脏标志留着", g_v.pkg_dirty, 1);
    ck("几何错算永久错→置坏", g_v.pkg_bad, 1);

    // ⑥ 第 1 层失败：量尺那一趟 + 备料那一趟各判一次
    casebeg("C5-2 第 1 层在量尺就错 ⇒ 0 条拷贝");
    clearday(); mk(&g_v, 16, 0x160, 256, 128, 1, 2, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_geom_fail_layer = 1;
    pkg_queue(&g_v); pkg_flush();
    ck("没录拷贝", ncopy, 0);
    ck("脏标志留着", g_v.pkg_dirty, 1);

    casebeg("C5-3 第 1 层在解排布时错 ⇒ 0 条拷贝（半张不许当新版本）");
    clearday(); mk(&g_v, 26, 0x260, 256, 128, 1, 2, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_dz_fail_layer = 1;
    pkg_queue(&g_v); pkg_flush();
    ck("没录拷贝", ncopy, 0);
    ck("解排布失败算永久错", g_v.pkg_bad, 1);
    ck("脏标志留着", g_v.pkg_dirty, 1);

    // ⑦ 量尺算少了（备料时圈位失败）：一条 copy 都不录，可重试
    casebeg("C5-4 备料圈位失败（量尺算少）⇒ 0 条拷贝、不置坏、卡环计数");
    clearday(); mk(&g_v, 17, 0x170, 256, 128, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    struct snap s0 = snap_get();
    g_rows_late = 4 * g_rows + 1024; g_late_after = 1;   // 第二趟（备料）改口：比量尺时多得多
    g_bigs_off = BIG_STAGE_SZ - 12000;                    // 余量只够量尺那 8 KB：备料一超量就圈不到
    pkg_queue(&g_v); pkg_flush();
    ck("没录拷贝", ncopy, 0);
    ck("没判死", g_v.pkg_bad, 0);
    ck("脏标志留着", g_v.pkg_dirty, 1);
    ck("卡环计数", D(s0, arena), 1);

    // ⑧ BC 解码失败：半张不许当新版本
    casebeg("C5-5 CPU 解压失败 ⇒ 0 条拷贝");
    clearday(); mk(&g_v, 18, 0x180, 256, 128, 2, 1, 1); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_dec_fail = 1;
    pkg_queue(&g_v); pkg_flush();
    ck("没录拷贝", ncopy, 0);
    ck("解压失败算永久错", g_v.pkg_bad, 1);
    ck("脏标志留着", g_v.pkg_dirty, 1);

    // ⑨ 成功必须覆盖全部子资源
    casebeg("C5-6 三级两层 ⇒ 六条拷贝，级/层各不重复");
    clearday(); mk(&g_v, 19, 0x190, 256, 128, 3, 2, 0); g_v.uploaded = 1; g_v.pkg_stale = 1;
    pkg_queue(&g_v); pkg_flush();
    ck("录满六条", ncopy, 6);
    for (int i = 0; i < ncopy; i++)
        for (int j = i + 1; j < ncopy; j++)
            if (cop[i].lvl == cop[j].lvl && cop[i].layer == cop[j].layer) { checks++; fails++; printf("【红】第 %d/%d 条重复\n", i, j); }
    ck("每条一图层", cop[0].cnt, 1);
    ck("清脏", g_v.pkg_dirty, 0);
    ck("清陈旧", g_v.pkg_stale, 0);
    ck("读回挂了两级", n_readarm, 2);

    // ⑩ 池尾之外：范围错要判死，且一条都不录
    casebeg("C5-7 池尾越界 ⇒ 0 条拷贝、判死");
    clearday(); mk(&g_v, 27, 0x270, 256, 128, 2, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_pool.size = 1024 * 8; g_base_step = 1024 * 8 * 3;   // 第 1 级起点已经出池
    pkg_queue(&g_v); pkg_flush();
    ck("没录拷贝", ncopy, 0);
    ck("越界算永久错", g_v.pkg_bad, 1);

    // ⑪ 换位：旧票据不得办到新对象身上（C4）
    casebeg("C4-1 swap-remove 后旧票据作废，新占位者的请求还在");
    clearday(); mk(&g_v, 20, 0x200, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    pkg_queue(&g_v);
    // 模拟 tex_release：把末尾那张搬到这一槽（同一块内存换了主人）
    mk(&g_v2, 30, 0x300, 32, 32, 1, 1, 0); g_v2.uploaded = 0; g_v2.pkg_dirty = 1;
    g_v = g_v2;
    struct snap m0 = snap_get();
    pkg_flush();
    ck("没进过上传", n_enter, 0);
    ck("换位计数", D(m0, moved), 1);
    ck("新占位者的脏还在", g_v.pkg_dirty, 1);
    ck("新占位者没被误标", g_v.uploaded, 0);
    pkg_queue(&g_v); pkg_flush();
    ck("按新身份重排就传上了", g_v.uploaded, 1);

    // ⑫ 复活：上一世的失败账不许带过来（C4，那几行也从源码抽）
    casebeg("C4-2 uploaded=0/bad=1 的旧账复活得清掉，否则这张永不再传");
    clearday(); mk(&g_v, 21, 0x210, 64, 64, 1, 1, 0);
    g_v.uploaded = 0; g_v.pkg_bad = 1; g_v.pkg_rej = 5; g_v.pkg_tried = 1;
#ifndef OLD
    g_v.pkg_next = 99999; g_v.pkg_why = STG_OVER;   // 上一世还欠着退避窗口：复活必须一并清掉
#endif
    memset(&g_td, 0, sizeof g_td); g_td.serial = 77; g_td.d.format = 0x25;
    g_td.mem = g_poolbytes; g_td.pool = &g_pool; g_td.d.width = 64; g_td.d.height = 64;
    { const tex_t *t = &g_td; vktex *v = &g_v;
#include "revive.inc"
    }
    ck("坏账清了", g_v.pkg_bad, 0);
    ck("拒绝账清了", g_v.pkg_rej, 0);
    ck("尝试账清了", g_v.pkg_tried, 0);
    ck("退避窗口也清了", (long long)v_next(&g_v), 0);
    ck("拒因也清了", v_why(&g_v), STG_OK);
    pkg_queue(&g_v);
    ck("这张现在排得进队", g_pkg_pend_n, 1);

    // ⑬ 当过附件的图不许把池内存倒灌回去（C3 mark_new_life 那条约束）
    casebeg("C3-5 drawn 的渲染目标：重初始化不打陈旧标志");
    clearday(); mk(&g_v, 22, 0x220, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.drawn = 1;
    tex_new_life(&g_v);
    ck("drawn=1 不倒灌", g_v.pkg_stale, 0);
    g_v.drawn = 0; tex_new_life(&g_v);
    ck("纯 CPU 供图才标陈旧", g_v.pkg_stale, 1);
    g_v.uploaded = 0; g_v.pkg_stale = 0; tex_new_life(&g_v);
    ck("没传过的无从谈陈旧", g_v.pkg_stale, 0);

    // ⑭ 延迟销毁：释放当场不许毁，等过一次提交才毁（C4 生命周期最后一条）
#ifndef OLD
    casebeg("C4-3 释放只进坟场：当场不毁，过一代才毁");
    clearday(); g_fence_val = 5;
    ck("开场坟场是空的", g_grave_n, 0);
    mk(&g_v, 40, 0x400, 64, 64, 1, 1, 0); g_v.mem = (VkDeviceMemory)(uintptr_t)0x700; g_v.vk_bytes = 12345;
    tex_retire(&g_v);
    ck("当场没毁视图", n_dview, 0);
    ck("当场没毁图", n_dimg, 0);
    ck("当场没释放显存", n_dmem, 0);
    ck("句柄已交出去（三个都清零）", !g_v.img && !g_v.view && !g_v.mem, 1);
    ck("vk_bytes 不动：回收站按它算预算", g_v.vk_bytes, 12345);
    ck("坟场里攒了一张", g_grave_n, 1);
    grave_reap();                              // 代号还没前进：没有任何提交被等完
    ck("同代一张不许毁", n_dimg, 0);
    g_fence_val = 6; grave_reap();
    ck("过一代毁视图", n_dview, 1);
    ck("过一代毁图", n_dimg, 1);
    ck("过一代释放显存", n_dmem, 1);
    ck("毁完坟场空了", g_grave_n, 0);
    n_dview = n_dimg = n_dmem = 0;
    mk(&g_v, 41, 0, 64, 64, 1, 1, 0);          // img/view/mem 全空
    tex_retire(&g_v);
    ck("空句柄不进坟场", g_grave_n, 0);
    ck("空句柄也没毁东西", n_dimg + n_dview + n_dmem, 0);
    mk(&g_v, 42, 0x420, 64, 64, 1, 1, 0); g_v.mem = (VkDeviceMemory)(uintptr_t)0x800;
    tex_retire(&g_v);
    g_fence_val = 7; grave_reap();
    ck("上一代的收掉了", g_grave_n, 0);
    ck("这一张毁了一次", n_dimg, 1);
    mk(&g_v, 43, 0x430, 64, 64, 1, 1, 0);
    tex_retire(&g_v);                          // 进在 7 代
    grave_reap();
    ck("当代的不许收", g_grave_n, 1);
    ck("当代的没被毁", n_dimg, 1);
    g_fence_val = 8; grave_reap();
    ck("下一代才收", g_grave_n, 0);
    printf("\n--- 坟场有界：堆满也不越界，只能提前毁最老的\n");
    n_dview = n_dimg = n_dmem = 0;
    for (int i = 0; i < GRAVE_N + 5; i++) {           // 代号一直不前进：一次提交都没等完
        mk(&g_v, 50 + i, (uint64_t)(0x1000 + i), 64, 64, 1, 1, 0);
        tex_retire(&g_v);
    }
    ck("只留 GRAVE_N 张", g_grave_n, GRAVE_N);
    ck("多出来的 5 张提前毁了", n_dimg, 5);
    ck("这几张没设 mem：不该调 vkFreeMemory", n_dmem, 0);
    g_fence_val++; grave_reap();
    ck("等过一次提交后剩下的全收", g_grave_n, 0);
    ck("收完累计毁了一张不落", n_dimg, GRAVE_N + 5);
#endif

    // ===== P2：环压力分档、退避、乘加判据、公平性与统计映射 =====

    casebeg("P2-1 余量不足：退避窗口内连量尺都不做，换条提交就救得回来");
    clearday(); mk(&g_v, 60, 0x600, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    struct snap a0 = snap_get();
    g_fence_val = 100;
    g_bigs_off = BIG_STAGE_SZ - 1024;             // 只剩 1 KB：这张量出来要 8 KB ⇒ 「本提交余量不足」
    pkg_queue(&g_v); pkg_flush();
    ck("拒因是余量不足", v_why(&g_v), STG_HEAD);
    ck("退到两代之后", (long long)v_next(&g_v), 102);
    ck("连拒一次", g_v.pkg_rej, 1);
    ck("分档记到余不", D(a0, head), 1);
    ck("总数仍留环拒", D(a0, big), 1);
    ck("没记到超容/无环", D(a0, over) + D(a0, nobuf), 0);
    ck("坏标志没动", g_v.pkg_bad, 0);
    ck("拷贝一条没录", ncopy, 0);
    ck("刚被拒的这张连票都没丢", g_pkg_pend_n, 1);
    ck("脏没清（请求还在）", g_v.pkg_dirty, 1);
    int e0 = n_enter;
    pkg_flush();                                  // 同一代再冲刷：退避窗口内直接跳过
    ck("第二次没去量尺", n_enter - e0, 0);
    ck("跳过记到退避", D(a0, bkoff), 1);
    ck("连拒没涨（不是忙等重试）", g_v.pkg_rej, 1);
    ck("票没了但请求还在资源身上", g_v.pkg_dirty, 1);
    mtl_sub_reset_bigstage(); g_fence_val += 4;   // 换提交：游标清零 + 退过窗口
    pkg_queue(&g_v); pkg_flush();
    ck("游标清零后传上了", g_v.uploaded, 1);
    ck("退避清零", (long long)v_next(&g_v), 0);
    ck("拒因清零", v_why(&g_v), STG_OK);
    ck("连拒清零", g_v.pkg_rej, 0);
    ck("环成记一笔", D(a0, bigok), 1);

    casebeg("P2-2 单图比整条环还大：不扩环也不判死，退避比余不足远 16 倍");
    clearday(); mk(&g_v, 61, 0x610, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_pool.size = 512ull << 20;                   // 池够用，才轮得到「环装不下」这一档
    g_rows = 210000;                              // 1024 x 210000 ≈ 205 MB > 整条 160 MB 环
    g_fence_val = 200;
    struct snap b0 = snap_get();
    pkg_queue(&g_v); pkg_flush();
    ck("拒因是超容", v_why(&g_v), STG_OVER);
    ck("分档记到超容", D(b0, over), 1);
    ck("没记到余不/无环", D(b0, head) + D(b0, nobuf), 0);
    ck("没判死", g_v.pkg_bad, 0);
    ck("一条没录", ncopy, 0);
    ck("脏没清", g_v.pkg_dirty, 1);
    ck("退得更远（2 代 x16）", (long long)v_next(&g_v), 232);

    casebeg("P2-2c 建环失败（分配失败）：记到无环、整趟停手，且分配只试一次不每图重试");
    clearday(); mk(&g_v, 62, 0x620, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    mk(&g_v2, 63, 0x630, 64, 64, 1, 1, 0); g_v2.uploaded = 1; g_v2.pkg_dirty = 1;
    g_ring_fail = 1; g_fence_val = 300;
    struct snap c0 = snap_get();
    pkg_queue(&g_v); pkg_queue(&g_v2); pkg_flush();
    ck("拒因是无环", v_why(&g_v), STG_NOBUF);
    ck("分档记到无环", D(c0, nobuf), 1);
    ck("没记到余不/超容", D(c0, head) + D(c0, over), 0);
    ck("第一张去量了尺", n_enter, 1);
    ck("建环失败就该整趟停手：第二张没试", g_v2.pkg_tried, 0);
    ck("两张都原样留队", g_pkg_pend_n, 2);
    ck("两张都没判死", !g_v.pkg_bad && !g_v2.pkg_bad, 1);
    ck("一条没录", ncopy, 0);
    ck("分配只试了一次（失败后不再每图重试）", n_mkbuf, 1);
    ck("退避比余不足远", (long long)(v_next(&g_v) - 300), 32);

    casebeg("P2-3a 块字节总量绕回：判死、0 拷贝、溢出计数");
    clearday(); mk(&g_v, 64, 0x640, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_rowb = 0xFFFFFFFFu; g_rows = 0xFFFFFFFFu; g_sl = 2;   // 块字节已经顶到 uint64 边上，再乘切片数就绕回
    struct snap d0 = snap_get();
    pkg_queue(&g_v); pkg_flush();
    ck("溢出计数", D(d0, ovfl), 1);
    ck("一条没录", ncopy, 0);
    ck("判死", g_v.pkg_bad, 1);
    ck("没装作成功（脏没清）", g_v.pkg_dirty, 1);
    ck("没圈位成功", D(d0, bigok), 0);

    casebeg("P2-3b 软解「行距×切片」绕回：同样判死、0 拷贝");
    clearday(); mk(&g_v, 65, 0x650, 65536, 65536, 1, 1, 1); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_pool.size = 1ull << 50; g_sl = 0xFFFFFFFFu;  // 池大到出不了范围，切片数让解压后的字节绕回
    struct snap e1 = snap_get();
    pkg_queue(&g_v); pkg_flush();
    ck("溢出计数", D(e1, ovfl), 1);
    ck("一条没录", ncopy, 0);
    ck("判死", g_v.pkg_bad, 1);
    ck("没圈位成功", D(e1, bigok), 0);

    casebeg("P2-4 队龄：32 代没传成要记账（这次照办），太老的让位但不丢请求");
    clearday(); mk(&g_v, 66, 0x660, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_fence_val = 1000; pkg_queue(&g_v);           // born = 1000
    g_fence_val += 100;                             // 32 ≤ 龄 < 128：记一笔，仍然办
    struct snap f0 = snap_get();
    pkg_flush();
    ck("队龄计数", D(f0, age), 1);
    ck("这张照办了", g_v.uploaded, 1);
    clearday(); mk(&g_v, 67, 0x670, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    g_fence_val = 2000; pkg_queue(&g_v);
    g_fence_val += 200;                             // 龄 ≥ 128：先让位给新图
    struct snap g0 = snap_get();
    pkg_flush();
    ck("太老的没去量尺", n_enter, 0);
    ck("队龄计数", D(g0, age), 1);
    ck("让位不等于丢请求（脏还在）", g_v.pkg_dirty, 1);
    ck("没判死", g_v.pkg_bad, 0);
    pkg_queue(&g_v); pkg_flush();                   // 绑定路径按新身份重排，born 重新起算
    ck("重排后传上了", g_v.uploaded, 1);

    casebeg("P2-5 一趟冲刷有上限：300 张只办 256，剩下的原样留队");
    clearday(); g_fence_val = 500;
    for (int i = 0; i < 300; i++) {
        mk(&g_many[i], (uint64_t)(700 + i), (uint64_t)(0x10000 + i), 64, 64, 1, 1, 0);
        g_many[i].uploaded = 1; g_many[i].pkg_dirty = 1;
        pkg_queue(&g_many[i]);
    }
    ck("都排进了队", g_pkg_pend_n, 300);
    pkg_flush();
    ck("这趟只办了上限张数", n_enter, 256);
    ck("剩下的留队", g_pkg_pend_n, 44);
    int done = 0;
    for (int i = 0; i < 300; i++) if (!g_many[i].pkg_dirty) done++;
    ck("办过的那 256 张脏清了", done, 256);
    pkg_flush();
    ck("下一趟接着办完", g_pkg_pend_n, 0);
    done = 0;
    for (int i = 0; i < 300; i++) if (!g_many[i].pkg_dirty) done++;
    ck("300 张最后全清了", done, 300);

    casebeg("P2-6 环上字节是接着用的：同一提交里两张图不会踩彼此的拷贝");
    clearday(); mk(&g_v, 68, 0x680, 64, 64, 1, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    mk(&g_v2, 69, 0x690, 64, 64, 1, 1, 0); g_v2.uploaded = 1; g_v2.pkg_dirty = 1;
    pkg_queue(&g_v); pkg_queue(&g_v2); pkg_flush();
    ck("两张都传上", g_v.uploaded && g_v2.uploaded, 1);
    ck("录了两条拷贝", ncopy, 2);
    ck("第二条的圈位接着第一条的尾巴（没覆盖）", cop[1].off >= cop[0].off + (uint64_t)g_rowb * g_rows, 1);

    casebeg("P2-7 统计逐项映射：标签后面的数必须是它自己那个计数器的值");
#ifndef OLD
    {
        for (int i = 0; i < EXP_N; i++) atomic_store(g_exp[i].c, (uint64_t)(1000 + i) << g_exp[i].shift);
        char line[4096]; memset(line, 0, sizeof line);
        int m = pkg_stat_line(line, (int)sizeof line, 0);   // 生产是从行中间续写：起点 0 也要能写进去
        ck("续写有产出", m > 0, 1);
        int seen[EXP_N]; for (int i = 0; i < EXP_N; i++) seen[i] = 0;
        int bad = 0, tok = 0;
        char cp[4096]; strncpy(cp, line, sizeof cp - 1); cp[sizeof cp - 1] = 0;
        for (char *p = strtok(cp, " "); p; p = strtok(NULL, " ")) {
            long num = -1; int which = tok_which(p, &num);
            tok++;
            if (which < 0) { printf("【红】token「%s」对不上任何标签\n", p); bad++; continue; }
            if (num != 1000 + which) { printf("【红】标签「%s」打出的数是 %ld，应是 %d（配错计数器）\n", g_exp[which].lab, num, 1000 + which); bad++; continue; }
            seen[which]++;
        }
        ck("每个 token 都归了档", bad, 0);
        ck("生产表条数和期望表一致", tok, EXP_N);
        for (int i = 0; i < EXP_N; i++) if (seen[i] != 1) { checks++; fails++; printf("【红】标签「%s」出现 %d 次（应为 1）\n", g_exp[i].lab, seen[i]); }
        checks += EXP_N;
    }
#else
    { checks++; fails++; printf("【红】旧版没有逐项映射表：统计全靠位置一一对应，第 40 轮那次串位就是这么漏的\n"); }
#endif

    // ⑯ P3 取证身份：CPU 落盘、整张落盘、GPU 读回三条证据必须钉在「同一个版本、同一层」上。
    // 版本代号或层号只要有一条错，离线联判就会把上一世的 CPU 字节配到这一世的读回上——四张产物看着齐全，结论全是假的。
    // The three capture probes must agree on which version and which layer they sampled; one wrong tag pairs a CPU buffer
    // from one upload with a GPU readback from another and the whole round's evidence is silently void.
    casebeg("P3-1 落盘/读回带同一个版本代号，重传后代号前进，层号两边一致");
    clearday(); mk(&g_v, 80, 0x800, 256, 128, 2, 1, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
    pkg_queue(&g_v); pkg_flush();
    ck("A/B 落了盘", n_dump, 2);                       // 级 0、1 各一次，第 2 级不留（省盘，也免得离线误配对）
    ck("整张落了盘", n_raw, 1);
    ck("落盘带版本代号", d_stamp != 0, 1);
    ck("读回带版本代号", r_stamp != 0, 1);
    ck("两条探针同一个版本", d_stamp == r_stamp, 1);
    ck("产物代号=资源在册的那一世", d_stamp == g_v.pkg_stamp, 1);
    ck("默认落第 0 层（落盘）", d_la, 0);
    ck("默认落第 0 层（读回）", r_la, 0);
    ck("落盘到最高那级为止", d_lvl, 1);
    uint64_t first_stamp = d_stamp;
    clearday(); g_v.pkg_dirty = 1;                     // 游戏又写了这页 ⇒ 再来一世，代号必须前进
    pkg_queue(&g_v); pkg_flush();
    ck("重传后代号变了", (long long)(d_stamp != first_stamp && d_stamp != 0), 1);
#ifndef OLD
    {
        clearday(); mk(&g_v, 81, 0x810, 256, 128, 2, 2, 0); g_v.uploaded = 1; g_v.pkg_dirty = 1;
        g_dump_layer = 1; g_read_layer = 1;            // DM_TEXDUMP_LAYER=1 DM_TEXREAD_LAYER=1
        pkg_queue(&g_v); pkg_flush();
        ck("开关指到哪层就落哪层（落盘）", d_la, 1);
        ck("开关指到哪层就采哪层（读回）", r_la, 1);
        ck("指层后只落那一层", n_dump, 2);
    }
#else
    { checks++; fails++; printf("【红】旧版探针不带版本代号/层号：ATLAS 之外没人知道这四张产物是不是同一世\n"); }
#endif

    printf("\n== 断言 %d 条｜红 %d 条\n", checks, fails);
    free(g_ring);
    return fails ? 1 : 0;
}
