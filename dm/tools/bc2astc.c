// bc2astc：把 KTEX 贴图里的 BC3 在构建机上一次性转成 ASTC 4x4，逐块原地换、文件字节数不变。
// 为什么能原地换：BC3 与 ASTC 4x4 都是 4×4 像素一块、每块 16 字节，块与块互不依赖，而 NVN 排布只是块的重排
// （第 13 轮实测：真机 A 产物的去重块集与 .tex 数据区的去重块集一致）⇒ 位置不用动，换块内容就行。
// 谁不转：8 字节块类（设备侧报的是 NVN 0x42 BC1）与未压缩类。头里 (bytes[4]>>4)&7 就是块字节数类：
// 0 = 8 字节、2 = 16 字节、5 = 未压缩；16 字节类实测全是 bytes[4]=0x20，与设备报过 NVN 0x44 的两个文件同字。
// 解码复用装机在跑的 dm/gfx/tex/nvn_bcdec.c（scripts/p3_bcref.sh 证过它与真机字节等价），不是另写一份。
// 宿主那边只把 NVN 0x44 的 Vk 格式改指 ASTC 4x4：块尺寸/行距/各级字节数都不变，搬块那一路原样能用。
//
// bc2astc: transcode the BC3 in KTEX textures to ASTC 4x4 once, on the build box, block by block in place
// (file size unchanged). Decoding reuses the shipped dm/gfx/tex/nvn_bcdec.c, not a second decoder.
// Usage: bc2astc --in <romfs> --out <dir> [--preset medium] [--jobs N] [--only SUB] [--limit N]
//                [--min-psnr 24] [--dry]     →  out/<relpath>、out/bake.tsv、out/summary.txt
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "astcenc.h"
#include "../gfx/tex/nvn_bcdec.h"

#define NVN_BC3 0x44u
#define BLK 16u                        // BC3 与 ASTC 4x4 都是每块 16 字节 | 16 bytes per block in both
#define BATCH (1u << 19)               // 每批块数：8 MB 密文 / 32 MB 像素 | blocks per batch
#define MAXITEM 20000

static char g_in[4096], g_out[4096], g_only[1024];
static unsigned g_jobs = 4, g_limit, g_dry;
static double g_quality = ASTCENC_PRE_MEDIUM, g_min_psnr = 24.0;
static uint64_t g_bytes_out;
static unsigned g_n_conv, g_n_refuse;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_tsv;
static char g_refus[MAXITEM][512];
static unsigned g_refus_n;

typedef struct { char rel[1024]; } item;
static item *g_items;
static unsigned g_nitems, g_next, g_binfo = 1;

static void refuse(const char *rel, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void refuse(const char *rel, const char *fmt, ...)
{
    char msg[384];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    pthread_mutex_lock(&g_mu);
    if (g_refus_n < MAXITEM) snprintf(g_refus[g_refus_n++], 512, "%s\t%s", rel, msg);
    g_n_refuse++;
    pthread_mutex_unlock(&g_mu);
}

static unsigned p16(unsigned n) { return (n + 15u) & ~15u; }

// 数据区起点 = 文件大小 − 各级块字节之和（块行距 = p16(ceil(宽/4)×16)，逐级折半到 1×1）
// Payload start = filesize - sum over mips of p16(ceil(w/4)*16) * ceil(h/4).
static uint64_t chain16(uint32_t w, uint32_t h)
{
    uint64_t s = 0; uint32_t a = w ? w : 1, b = h ? h : 1;
    for (;;) {
        s += (uint64_t)p16(((a + 3u) / 4u) * BLK) * ((b + 3u) / 4u);
        if (a == 1u && b == 1u) break;
        a = a > 1u ? a >> 1 : 1u; b = b > 1u ? b >> 1 : 1u;
    }
    return s;
}

static int ensuredirs(char *path)
{
    for (char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        if (mkdir(path, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    return 0;
}

static void walk(const char *root, const char *rel)
{
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char abspath[4200], nrel[1024];
        snprintf(abspath, sizeof abspath, "%s/%s", root, e->d_name);
        if (rel[0]) snprintf(nrel, sizeof nrel, "%s/%s", rel, e->d_name);
        else snprintf(nrel, sizeof nrel, "%s", e->d_name);
        struct stat st;
        if (stat(abspath, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk(abspath, nrel); continue; }
        size_t L = strlen(nrel);
        if (L < 4 || strcmp(nrel + L - 4, ".tex")) continue;
        if (g_only[0] && !strstr(nrel, g_only)) continue;
        if (g_nitems < MAXITEM) { strcpy(g_items[g_nitems].rel, nrel); g_nitems++; }
    }
    closedir(d);
}

typedef struct {
    struct astcenc_context *enc, *dec;
    uint8_t *rgba, *back, *astc;       // 真值像素 / 解回像素 / 密文 | truth texels, re-decoded texels, blocks
    void *slices[1];
    void *rslices[1];
} wctx;

static void work(int widx, wctx *wc)
{
    char src[4200], dst[4200];
    for (;;) {
        unsigned i;
        pthread_mutex_lock(&g_mu);
        i = g_next++;
        pthread_mutex_unlock(&g_mu);
        if (i >= g_nitems || (g_limit && i >= g_limit)) return;
        snprintf(src, sizeof src, "%s/%s", g_in, g_items[i].rel);
        struct stat st;
        if (stat(src, &st) || !S_ISREG(st.st_mode) || st.st_size < 64) continue;
        FILE *f = fopen(src, "rb");
        if (!f) continue;
        size_t sz = (size_t)st.st_size;
        uint8_t *buf = malloc(sz);
        if (!buf) { fclose(f); continue; }
        if (fread(buf, 1, sz, f) != sz) { fclose(f); free(buf); continue; }
        fclose(f);

        if (memcmp(buf, "KTEX", 4)) { refuse(g_items[i].rel, "不是 KTEX"); free(buf); continue; }
        unsigned cls = (buf[4] >> 4) & 7u;
        if (cls != 2u) { refuse(g_items[i].rel, "块类%u（只转 16 字节块）", cls); free(buf); continue; }
        if (buf[4] != 0x20u) { refuse(g_items[i].rel, "16 字节类里的新标记 bytes[4]=0x%02x", buf[4]); free(buf); continue; }
        uint32_t w = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8), h = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8);
        uint64_t ch = chain16(w, h);
        // 两种几何：多数 .tex 带完整 mip 链（一直到 1×1）；实测有 3 张只到第 0 级 —— 头 @12 就是块行距，
        // 单级字节 = 行距 × ceil(高/4)。全链放不下的时候才按单级算，别把小的 4×4 那种误判成单级。
        // Most .tex carry the full mip chain; three of them stop at level 0 (level-0 bytes = the @12 pitch
        // times the block-row count). Only fall back to one level when the full chain cannot fit the file.
        uint64_t lv0 = (uint64_t)p16(((w + 3u) / 4u) * BLK) * ((h + 3u) / 4u);
        const char *geo = "链";
        uint64_t endp = ch;
        if (ch > sz || sz - ch < 16u) { geo = "单"; endp = lv0; }
        // 校验的是负载本身：endp 必须 16 整除且不超过文件；头 = sz-endp，允许 18 这类非 16 对齐的长度。
        // The payload is what must be block-aligned; the header (sz-endp) may be 18 or any other non-aligned length.
        if (endp > sz || endp % BLK || sz - endp < 4u) {
            refuse(g_items[i].rel, "几何不吻合 %ux%u 链%llu 单%llu 文件%zu", w, h, (unsigned long long)ch,
                   (unsigned long long)lv0, sz);
            free(buf); continue;
        }
        size_t ps = (size_t)(sz - endp);
        uint64_t nblk = (sz - ps) / BLK, done = 0;
        double sse[4] = { 0, 0, 0, 0 };
        uint64_t texels = 0;
        int fail = 0;

        while (done < nblk) {
            unsigned k = (unsigned)(nblk - done > BATCH ? BATCH : nblk - done);
            uint8_t *p = buf + ps + (size_t)done * BLK;
            for (unsigned b = 0; b < k; b++)
                if (nvn_bc_decode(NVN_BC3, p + (size_t)b * BLK, BLK, 4, 4, wc->rgba + (size_t)b * 64, 16)) {
                    refuse(g_items[i].rel, "BC3 解码失败"); fail = 1; break;
                }
            if (fail) break;
            wc->slices[0] = wc->rgba;
            struct astcenc_image img = { .dim_x = 4, .dim_y = 4u * k, .dim_z = 1, .data_type = ASTCENC_TYPE_U8, .data = wc->slices };
            const struct astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
            enum astcenc_error err = astcenc_compress_image(wc->enc, &img, &swz, wc->astc, (size_t)k * BLK, 0);
            if (err != ASTCENC_SUCCESS) { refuse(g_items[i].rel, "编码失败：%s", astcenc_get_error_string(err)); fail = 1; break; }
            wc->rslices[0] = wc->back;
            struct astcenc_image out = { .dim_x = 4, .dim_y = 4u * k, .dim_z = 1, .data_type = ASTCENC_TYPE_U8, .data = wc->rslices };
            err = astcenc_decompress_image(wc->dec, wc->astc, (size_t)k * BLK, &out, &swz, 0);
            if (err != ASTCENC_SUCCESS) { refuse(g_items[i].rel, "解回失败：%s", astcenc_get_error_string(err)); fail = 1; break; }
            for (size_t t = 0; t < (size_t)k * 64; t++) {
                double d = (double)wc->back[t] - (double)wc->rgba[t];
                sse[t & 3] += d * d;
            }
            texels += (uint64_t)k * 16;
            memcpy(p, wc->astc, (size_t)k * BLK);   // 原地写回：块数、块序、偏移都不变 | in place, same order
            if (done == 0 && g_binfo) {   // 产物结构自检：头一块要真是 ASTC 4x4 单层 LDR，错产物在真机上是静默作废一整轮
                struct astcenc_block_info bi;
                if (astcenc_get_block_info(wc->dec, wc->astc, &bi) != ASTCENC_SUCCESS) {
                    fprintf(stderr, "块结构自检：这份 astcenc 不给出块属性，跳过（不影响产物）\n");
                } else if (bi.block_x != 4 || bi.block_y != 4 || bi.block_z != 1 || bi.profile != ASTCENC_PRF_LDR) {
                    refuse(g_items[i].rel, "产物块不是 ASTC 4x4 LDR（%u x %u x %u 档 %d）",
                           bi.block_x, bi.block_y, bi.block_z, (int)bi.profile);
                    fail = 1; break;
                } else {
                    fprintf(stderr, "块结构自检：ASTC %u x %u x %u 档 %d 平面 %d 常量块 %d\n",
                            bi.block_x, bi.block_y, bi.block_z, (int)bi.profile, (int)bi.is_dual_plane_block,
                            (int)bi.is_constant_block);
                }
                g_binfo = 0;   // 一次运行验一次就够：这是「产物长什么样」的探针，不是每块都要过的关
            }
            done += k;
        }
        if (fail) { free(buf); continue; }

        uint64_t n4 = texels * 4;
        double mse = n4 ? (sse[0] + sse[1] + sse[2] + sse[3]) / (double)n4 : 0;
        double psnr = mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
        double rmse_a = texels ? sqrt(sse[3] / (double)texels) : 0;
        if (psnr < g_min_psnr) { refuse(g_items[i].rel, "PSNR %.1f 低于 %.1f 不写", psnr, g_min_psnr); free(buf); continue; }

        if (!g_dry) {
            snprintf(dst, sizeof dst, "%s/%s", g_out, g_items[i].rel);
            if (ensuredirs(dst)) { refuse(g_items[i].rel, "建目录失败 errno%d", errno); free(buf); continue; }
            FILE *o = fopen(dst, "wb");
            if (!o || fwrite(buf, 1, sz, o) != sz) { refuse(g_items[i].rel, "写产物失败"); if (o) fclose(o); free(buf); continue; }
            fclose(o);
            if (stat(dst, &st) || (size_t)st.st_size != sz) { refuse(g_items[i].rel, "产物大小变了"); free(buf); continue; }
        }
        pthread_mutex_lock(&g_mu);
        g_n_conv++; g_bytes_out += sz;
        if (g_tsv) fprintf(g_tsv, "%s\t%zu\t%zu\t%llu\t%.2f\t%.3f\t%.0f\t%s\n", g_items[i].rel, sz, ps,
                           (unsigned long long)nblk, psnr, rmse_a, g_quality, geo);
        pthread_mutex_unlock(&g_mu);
        free(buf);
    }
}

static void *th(void *p)
{
    int widx = *(int *)p;
    wctx *wc = calloc(1, sizeof *wc);
    struct astcenc_config cfg;
    if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, (float)g_quality, 0, &cfg) != ASTCENC_SUCCESS) { fprintf(stderr, "编码 config 失败\n"); exit(2); }
    if (astcenc_context_alloc(&cfg, 1, &wc->enc, NULL) != ASTCENC_SUCCESS) { fprintf(stderr, "编码上下文失败\n"); exit(2); }
    struct astcenc_config dcfg;
    if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, (float)g_quality, ASTCENC_FLG_DECOMPRESS_ONLY, &dcfg) != ASTCENC_SUCCESS) { fprintf(stderr, "解码 config 失败\n"); exit(2); }
    if (astcenc_context_alloc(&dcfg, 1, &wc->dec, NULL) != ASTCENC_SUCCESS) { fprintf(stderr, "解码上下文失败\n"); exit(2); }
    wc->rgba = malloc((size_t)BATCH * 64);
    wc->back = malloc((size_t)BATCH * 64);
    wc->astc = malloc((size_t)BATCH * BLK);
    if (!wc->rgba || !wc->back || !wc->astc) { fprintf(stderr, "内存不够\n"); exit(2); }
    work(widx, wc);
    free(wc->rgba); free(wc->back); free(wc->astc);
    astcenc_context_free(wc->enc); astcenc_context_free(wc->dec); free(wc);
    return NULL;
}

static double now(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + 1e-6 * (double)tv.tv_usec;
}

int main(int argc, char **argv)
{
    g_items = calloc(MAXITEM, sizeof *g_items);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--in") && i + 1 < argc) snprintf(g_in, sizeof g_in, "%s", argv[++i]);
        else if (!strcmp(a, "--out") && i + 1 < argc) snprintf(g_out, sizeof g_out, "%s", argv[++i]);
        else if (!strcmp(a, "--only") && i + 1 < argc) snprintf(g_only, sizeof g_only, "%s", argv[++i]);
        else if (!strcmp(a, "--jobs") && i + 1 < argc) g_jobs = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--limit") && i + 1 < argc) g_limit = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--min-psnr") && i + 1 < argc) g_min_psnr = atof(argv[++i]);
        else if (!strcmp(a, "--dry")) g_dry = 1;
        else if (!strcmp(a, "--preset") && i + 1 < argc) {
            const char *q = argv[++i];
            g_quality = !strcmp(q, "fastest") ? ASTCENC_PRE_FASTEST : !strcmp(q, "fast") ? ASTCENC_PRE_FAST
                      : !strcmp(q, "medium") ? ASTCENC_PRE_MEDIUM : !strcmp(q, "thorough") ? ASTCENC_PRE_THOROUGH
                      : !strcmp(q, "verythorough") ? ASTCENC_PRE_VERYTHOROUGH : atof(q);
        } else {
            fprintf(stderr, "用法：%s --in <romfs> --out <dir> [--preset medium] [--jobs N] [--only SUB] [--limit N] [--min-psnr 24] [--dry]\n", argv[0]);
            return 2;
        }
    }
    if (!g_in[0] || !g_out[0]) { fprintf(stderr, "要 --in 与 --out\n"); return 2; }
    walk(g_in, "");
    fprintf(stderr, "扫到 .tex %u 个 / %u 路 / 档位 %.0f / 门槛 PSNR %.1f%s\n",
            g_nitems, g_jobs, g_quality, g_min_psnr, g_dry ? "（试跑不写）" : "");
    if (!g_dry) {
        if (mkdir(g_out, 0755) && errno != EEXIST) { perror("mkdir out"); return 2; }
        char t[4200]; snprintf(t, sizeof t, "%s/bake.tsv", g_out);
        g_tsv = fopen(t, "w");
        if (g_tsv) fprintf(g_tsv, "rel\tbytes\tpayload\tblocks\tpsnr\trmse_a\tpreset\n");
    }
    double t0 = now();
    pthread_t *ids = calloc(g_jobs ? g_jobs : 1, sizeof *ids);
    int *ix = calloc(g_jobs ? g_jobs : 1, sizeof *ix);
    for (unsigned i = 0; i < g_jobs; i++) { ix[i] = (int)i; pthread_create(&ids[i], NULL, th, &ix[i]); }
    for (unsigned i = 0; i < g_jobs; i++) pthread_join(ids[i], NULL);
    if (g_tsv) { fclose(g_tsv); g_tsv = NULL; }
    double dt = now() - t0;
    char s[4200]; snprintf(s, sizeof s, "%s/summary.txt", g_out);
    FILE *o = fopen(s, "w");
    if (o) {
        fprintf(o, "转成 %u 张 / %llu 字节 / %.0f 秒（%.1f MB/秒）；拒绝 %u\n", g_n_conv,
                (unsigned long long)g_bytes_out, dt, (double)(g_bytes_out >> 20) / (dt > 0 ? dt : 1), g_n_refuse);
        for (unsigned i = 0; i < g_refus_n; i++) fprintf(o, "拒\t%s\n", g_refus[i]);
        fclose(o);
    }
    fprintf(stderr, "转成 %u 张 %llu MB / %.0f 秒，拒绝 %u 个（明细见 summary.txt）\n", g_n_conv,
            (unsigned long long)(g_bytes_out >> 20), dt, g_n_refuse);
    for (unsigned i = 0; i < g_refus_n && i < 20; i++) fprintf(stderr, "  拒 %s\n", g_refus[i]);
    return 0;
}
