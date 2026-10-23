// bc1_offline：把装机那条「BC 块 → CPU 软解 → 上传」的路，在构建机/本机 WSL 上原样跑一遍给定清单里的
// 8 字节块类贴图（KTEX 头 bytes[4] 高位 = 0：BC1 / BC4 都是这个类），逐级解出线性块与 RGBA 落盘，离线判。
// 为什么要跑生产那三份源码而不是另写一份：布局 nvn_tex.c、解排布 nvn_deswizzle.c、解压 nvn_bcdec.c 都是
// HAP 里在跑的那一份；离线复刻一旦自己写，判红的可能是我们自己的复刻（P3 那晚 565 端点抄反就是这么抓到的）。
//
// bc1_offline: run the shipped package→deswizzle→CPU-decode path offline over a list of 8-bytes-per-block
// KTEX textures. It links the very sources that ship in the HAP, so a red verdict means the device build is
// red - not that our second implementation disagrees.
//
// Usage: bc1_offline --in <romfs 根> --list <清单：每行一个相对路径> --out <目录>
//                    [--fmt 0x42] [--pass 0] [--levels 0=自算] [--keep]
//   --pass N  强制预排布块高参数（pkg_block_h，0 = 用 nvn_tex.c 的自动算法）
//   产出：<out>/index.tsv、<out>/<id>.L0.blk|.rgba、<out>/<id>.L1.blk|.rgba、<out>/<id>.geom.txt
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../gfx/nvn/nvn_tex.h"
#include "../gfx/tex/nvn_bcdec.h"
#include "../gfx/tex/nvn_deswizzle.h"

#define MAXF 20000

static char g_in[4096], g_out[4096], g_list[4096];
static unsigned g_fmt = 0x42u;
static int g_pass = -1, g_levels;
static int g_showgeom;

// 一条 index.tsv 记录的所有可填字段；未知一律留 0，靠 emit() 固定列序输出
// Every fillable field of one index.tsv row; unknowns stay 0 and emit() fixes the column order.
typedef struct {
    const char *rel, *id, *tab;
    long long bytes;
    unsigned b4, b5, w, h, fmt, tileH, levels;
    long long bpb, pass, hdr;
    long long prod, diff;                 // 生产布局总字节、与文件负载的差
    long long l1prod, l1file;             // 级 1 起点：生产算的 vs 文件表算的
    unsigned l0rb, l0rows;
    long long blk0, px0, blk1, px1;
} rec_t;

static void emit(FILE *tf, const rec_t *r, const char *status)
{
    fprintf(tf, "%s\t%s\t%lld\t0x%02x\t%u\t%u\t%u\t0x%x\t%lld\t%lld\t%u\t%u\t%lld\t%s\t%lld\t%lld\t%lld\t%lld\t"
                "%u\t%u\t%lld\t%lld\t%lld\t%lld\t%s\n",
            r->rel, r->id ? r->id : "", r->bytes, r->b4, r->b5, r->w, r->h, r->fmt, r->bpb, r->pass,
            r->tileH, r->levels, r->hdr, r->tab ? r->tab : "", r->prod, r->diff, r->l1prod, r->l1file,
            r->l0rb, r->l0rows, r->blk0, r->px0, r->blk1, r->px1, status);
}

static void slug(const char *rel, char *o, size_t n)
{
    size_t k = 0;
    for (const char *p = rel; *p; p++) {
        char c = (*p == '/' || *p == '\\') ? '_' : *p;
        if (k + 8 >= n) break;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        o[k++] = c;
    }
    size_t L = strlen(o);
    if (L > 4 && !strcmp(o + L - 4, ".tex")) o[L - 4] = 0;
}

// 一个候选布局能不能把这张文件装下：返回头长（文件字节 − 布局总字节），负数 = 装不下
// Does this candidate layout fit the file? Returns the implied header length, negative if it cannot fit.
static long long fit(const nvn_texdesc *d, unsigned levels, uint64_t *size, unsigned *tileH)
{
    nvn_texdesc c = *d;
    c.levels = levels ? levels : 1;
    nvn_texlayout lo;
    nvn_tex_layout(&c, &lo);
    if (size) *size = lo.size;
    if (tileH) *tileH = lo.tileH;
    return -(long long)lo.size;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   // --geom 的逐行探针要在崩溃时也留得住
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc) snprintf(g_in, sizeof g_in, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) snprintf(g_out, sizeof g_out, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--list") && i + 1 < argc) snprintf(g_list, sizeof g_list, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--fmt") && i + 1 < argc) g_fmt = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--pass") && i + 1 < argc) g_pass = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--levels") && i + 1 < argc) g_levels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--geom")) g_showgeom = 1;
        else { fprintf(stderr, "不认识参数：%s\n", argv[i]); return 2; }
    }
    if (!g_in[0] || !g_out[0] || !g_list[0]) {
        fprintf(stderr, "用法：bc1_offline --in <romfs> --list <清单> --out <目录> [--fmt 0x42] [--pass N] [--levels N]\n");
        return 2;
    }
    mkdir(g_out, 0777);

    FILE *lf = fopen(g_list, "r");
    if (!lf) { fprintf(stderr, "清单打不开：%s（%s）\n", g_list, strerror(errno)); return 1; }
    char tsv[4200];
    snprintf(tsv, sizeof tsv, "%s/index.tsv", g_out);
    FILE *tf = fopen(tsv, "w");
    fprintf(tf, "rel\tid\tbytes\tb4\tb5\tw\th\tfmt\tbpb\tpass\ttileH\tlevels\thdr\tKTEX表\t"
                "生产布局字节\t生产−负载\t级1起点_生产\t级1起点_文件\tL0行\tL0块行\t"
                "L0块字节\tL0像素字节\tL1块字节\tL1像素字节\t状态\n");

    char line[1024];
    int n = 0, ok = 0, bad = 0;
    while (fgets(line, sizeof line, lf)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!line[0]) continue;
        if (n++ >= MAXF) break;
        rec_t R;
        memset(&R, 0, sizeof R);
        R.rel = line; R.fmt = g_fmt; R.pass = g_pass;
        char src[5300];
        snprintf(src, sizeof src, "%s/%s", g_in, line);
        struct stat st;
        if (stat(src, &st) || !S_ISREG(st.st_mode)) {
            emit(tf, &R, "缺文件");
            bad++; continue;
        }
        FILE *f = fopen(src, "rb");
        if (!f) { emit(tf, &R, "打不开"); bad++; continue; }
        uint8_t *buf = malloc((size_t)st.st_size);
        if (!buf || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
            free(buf); fclose(f);
            R.bytes = (long long)st.st_size;
            emit(tf, &R, "读不完整"); bad++; continue;
        }
        fclose(f);
        size_t sz = (size_t)st.st_size;
        if (sz < 32 || memcmp(buf, "KTEX", 4)) {
            free(buf);
            R.bytes = (long long)st.st_size;
            emit(tf, &R, "不是KTEX"); bad++; continue;
        }
        unsigned w = buf[8] | (buf[9] << 8), h = buf[10] | (buf[11] << 8);
        R.bytes = (long long)sz; R.b4 = buf[4]; R.b5 = buf[5];
        R.w = w; R.h = h;
        unsigned lv_calc = 1, mw = w > h ? w : h;
        while (mw > 1) { mw >>= 1; lv_calc++; }
        unsigned lv = (unsigned)(g_levels ? g_levels : (int)lv_calc);

        // KTEX 自带的逐级表（权威几何）：头 = 8 + 10×级，每级 10 字节 {u16 宽, u16 高, u16 块行距, u32 本级字节}，
        // 负载就是这些本级字节首尾相接。表里的行距是**未对齐到 64 字节的线性行距**，而生产布局 nvn_tex.c 算的是
        // 64B×8 行的 GOB 块线性——两者对不上时，差的就是「上传时按哪套排布」。这张表把级数和头长都钉死，
        // 剩下的比较才只回答排布这一个问题。
        unsigned cls = (buf[4] >> 4) & 7u;
        int hbpb = cls == 0u ? 8 : cls == 2u ? 16 : 0;
        int tab_lv = -1;
        long long tab_hdr = -1;
        if (hbpb) {
            for (int k = 1; k <= 24; k++) {
                long long hh = 8 + 10 * (long long)k;
                if (hh > (long long)sz) break;
                uint64_t acc = 0;
                int good = 1;
                for (int l = 0; l < k; l++) {
                    const uint8_t *e = buf + 8 + 10 * l;
                    unsigned ew = (unsigned)(e[0] | (e[1] << 8)), eh = (unsigned)(e[2] | (e[3] << 8));
                    unsigned ep = (unsigned)(e[4] | (e[5] << 8));
                    uint64_t es = (uint64_t)e[6] | ((uint64_t)e[7] << 8) | ((uint64_t)e[8] << 16) | ((uint64_t)e[9] << 24);
                    unsigned dw = w >> l ? w >> l : 1u, dh = h >> l ? h >> l : 1u;
                    uint64_t want = (uint64_t)(((dw + 3u) / 4u) * hbpb) * ((dh + 3u) / 4u);
                    if (ew != dw || eh != dh || (uint64_t)ep != (uint64_t)((dw + 3u) / 4u) * hbpb ||
                        es != want || (uint64_t)sz - (uint64_t)hh < acc + want) { good = 0; break; }
                    acc += want;
                }
                if (good && (uint64_t)sz - (uint64_t)hh == acc) { tab_lv = k; tab_hdr = hh; break; }
            }
        }
        const char *tab = tab_lv > 0 ? "表✓" : "表✗";
        if (tab_lv > 0 && !g_levels) lv = (unsigned)tab_lv;

        nvn_texdesc d;
        memset(&d, 0, sizeof d);
        d.target = 2;                              // NVNtextureTarget2D
        d.format = (int)g_fmt;
        d.flags = 0; d.samples = 0;
        d.width = w; d.height = h; d.depth = 1; d.levels = lv; d.stride = 0;
        d.pkg_block_h = g_pass > 0 ? g_pass : 0;

        // 两套几何各算一份，摆在一起判：生产 = GOB 块线性（nvn_tex.c，HAP 里在跑的那份），
        // 文件 = KTEX 表里的线性行距。级 1 起点不一样就说明上传按哪套排布这件事还没定。
        uint64_t lsize = 0; unsigned tileH = 0;
        (void)fit(&d, lv, &lsize, &tileH);
        long long hdr = tab_hdr >= 0 ? tab_hdr : (long long)sz - (long long)lsize;
        long long l1prod = lv > 1 ? (long long)nvn_tex_level_offset(&d, 1) : 0;
        long long l1file = hbpb ? (long long)(((w + 3u) / 4u) * (unsigned)hbpb) * ((h + 3u) / 4u) : 0;
        R.tab = tab; R.prod = (long long)lsize; R.diff = (long long)lsize - (long long)(sz - (tab_hdr >= 0 ? tab_hdr : 0));
        R.l1prod = l1prod; R.l1file = l1file;
        if (g_showgeom) {
            printf("%s  %ux%u  级%u  b4=0x%02x b5=0x%02x %s  文件%zu  负载%lld  生产布局%llu  差%lld  "
                   "头%lld  tileH=%u  级1起点 生产%lld/文件%lld\n",
                   line, w, h, lv, buf[4], buf[5], tab, sz, (long long)sz - hdr,
                   (unsigned long long)lsize, R.diff, hdr, tileH, l1prod, l1file);
        }
        if (hdr < 0 || hdr > 2048) {
            R.levels = lv; R.tileH = tileH; R.hdr = hdr;
            emit(tf, &R, "几何不吻合");
            free(buf);
            bad++; continue;
        }

        int bpb = 0, bw = 1, bh = 1;
        bpb = nvn_fmt_bpb((int)g_fmt, &bw, &bh);
        uint32_t upf = 0; int bpp = 0;
        R.levels = lv; R.tileH = tileH; R.hdr = hdr; R.bpb = bpb;
        if (!nvn_bc_unpacked(g_fmt, &upf, &bpp)) {
            free(buf);
            emit(tf, &R, "不是BC");
            bad++; continue;
        }

        uint8_t *storage = buf + hdr;
        char id[512]; slug(line, id, sizeof id);
        R.id = id;

        unsigned l0rb = 0, l0rows = 0, l0slices = 0, lth = 0, ltd = 0; uint32_t lbpb = 0;
        uint64_t l0base = 0;
        if (nvn_deswizzle_geom(&d, 0, 0, &l0rb, &l0rows, &l0slices, &l0base, &lth, &ltd, &lbpb)) {
            emit(tf, &R, "量不出几何");
            free(buf);
            bad++; continue;
        }
        size_t lvlsz[2] = { 0, 0 };
        uint8_t *prgb[2] = { NULL, NULL };
        size_t prow[2] = { 0, 0 };
        unsigned lwh[2][2];

        int softfail = 0;
        for (unsigned l = 0; l <= 1 && l < lv; l++) {
            unsigned ew = w >> l ? w >> l : 1u, eh = h >> l ? h >> l : 1u;
            uint32_t rb = 0, rows = 0, slices = 0, bp2 = 0; unsigned th = 0, td = 0;
            uint64_t bs = 0;
            if (nvn_deswizzle_geom(&d, l, 0, &rb, &rows, &slices, &bs, &th, &td, &bp2) || !rb) { softfail = 1; break; }

            lwh[l][0] = ew; lwh[l][1] = eh;
            // 这套布局要吃到的字节必须落在文件里：越界=几何猜错，不能照读（读越界是段错误，判红也就没了意义）
            if ((uint64_t)hdr + bs + (uint64_t)rb * rows > sz) { softfail = 4; break; }
            uint8_t *lb = malloc((size_t)rb * rows + 64);
            if (nvn_deswizzle(&d, l, 0, storage, lb, (size_t)rb)) { softfail = 2; free(lb); break; }
            size_t blk = (size_t)rb * rows;
            uint64_t dr = (((uint64_t)ew * (uint64_t)bpp) + 15) & ~(uint64_t)15;
            uint8_t *px = malloc((size_t)dr * eh + 64);
            if (nvn_bc_decode(g_fmt, lb, (size_t)rb, ew, eh, px, (size_t)dr)) { softfail = 3; free(lb); free(px); break; }
            prgb[l] = px; prow[l] = (size_t)dr; lvlsz[l] = blk;
            char p[4700];
            snprintf(p, sizeof p, "%s/%s.L%u.blk", g_out, id, l);
            FILE *o = fopen(p, "wb"); if (o) { fwrite(lb, 1, blk, o); fclose(o); }
            snprintf(p, sizeof p, "%s/%s.L%u.rgba", g_out, id, l);
            o = fopen(p, "wb"); if (o) { fwrite(px, 1, (size_t)dr * eh, o); fclose(o); }
            free(lb);
        }
        // 级 0 的像素行距与宽高记进 tsv，离线那份 python 靠这几个数把块流拼成 DDS 当独立神谕
        R.l0rb = l0rb; R.l0rows = l0rows;
        R.blk0 = (long long)lvlsz[0]; R.px0 = prgb[0] ? (long long)(prow[0] * lwh[0][1]) : 0;
        R.blk1 = (long long)lvlsz[1]; R.px1 = prgb[1] ? (long long)(prow[1] * lwh[1][1]) : 0;
        emit(tf, &R, softfail ? (softfail == 2 ? "解排布失败" : softfail == 3 ? "解码失败"
                               : softfail == 4 ? "存储越界" : "几何失败") : "OK");
        for (unsigned l = 0; l < 2; l++) free(prgb[l]);
        free(buf);
        if (softfail) bad++; else ok++;
    }
    fclose(tf); fclose(lf);
    printf("== bc1_offline：格式 0x%x｜判 %d 张｜OK %d｜红 %d｜清单 %s\n", g_fmt, n, ok, bad, tsv);
    return bad ? 1 : 0;
}
