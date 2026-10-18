// P3 第一档判据的地基自检：离线那份 BC 参考解码器必须和生产装机那份逐字节一样。
// scripts/p3_join.py 里「CPU 已坏」这一档整条压在「离线参考解码器是对的」上：它要是歪了，正确的产物会被判成坏像素，
// 一整轮真机证据就白拿。这里用同一条伪随机块流（同一个 LCG），交生产 nvn_bc_decode 解一遍、把块流与解出的 RGBA 都落盘，
// 离线那份再解一遍，cmp 判等。四种格式 x 三种尺寸，边上不满 4x4 的块也要走到。
// The offline reference decoder in p3_join.py decides the first rung of the P3 ladder, so it is checked byte for byte
// against the decoder that actually ships: one deterministic block stream, decoded by both, compared with cmp.
// 跑法 | run: scripts/p3_bcref.sh（宿主 cc 即可，不需要设备）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../tex/nvn_bcdec.h"

static unsigned g_s;
static unsigned char nx(void) { g_s = g_s * 1103515245u + 12345u; return (unsigned char)((g_s >> 16) & 0xFF); }

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "用法 usage: %s <NVN格式> <w> <h> <blocks.bin> <rgba.bin>\n", argv[0]); return 2; }
    unsigned f = (unsigned)strtoul(argv[1], NULL, 0);
    uint32_t w = (uint32_t)strtoul(argv[2], NULL, 0), h = (uint32_t)strtoul(argv[3], NULL, 0);
    int bb = (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46) ? 8 : 16;   // 与 nvn_bc_decode 同一句判据
    uint32_t bpr = (w + 3) / 4, rows = (h + 3) / 4;
    size_t pitch = (size_t)bpr * bb, bsz = pitch * rows;
    uint32_t dp = (((uint32_t)w * 4u) + 15) & ~(uint32_t)15;                 // RGBA8 行距：16 对齐
    size_t dsz = (size_t)dp * h;
    uint8_t *bl = malloc(bsz), *dst = calloc(1, dsz);
    if (!bl || !dst) { fprintf(stderr, "没分到内存\n"); return 1; }
    g_s = f * 2654435761u + w * 40503u + h;                                  // 种子跟着这组参数走：每格都是不同的料
    for (size_t i = 0; i < bsz; i++) bl[i] = nx();
    if (nvn_bc_decode(f, bl, pitch, w, h, dst, dp)) { fprintf(stderr, "格式 0x%x 生产解码器不接\n", f); return 3; }
    FILE *a = fopen(argv[4], "wb"), *b = fopen(argv[5], "wb");
    if (!a || !b) { fprintf(stderr, "写不出 %s / %s\n", argv[4], argv[5]); return 1; }
    fwrite(bl, 1, bsz, a); fclose(a);
    fwrite(dst, 1, dsz, b); fclose(b);
    printf("ok 0x%x %ux%u 块行%zu 像素行%u 块%zu 解后%zu\n", f, w, h, pitch, dp, bsz, dsz);
    free(bl); free(dst);
    return 0;
}
