// 构建机折出的着色器二进制库（shaders.bin）布局：设备端只按这里的结构读，不解析 JSON。
// 生成脚本 shbin_gen.py（在构建机上跑，输入 dmsh_build.py 的 SPVOUT 产物 + 设备报出的键清单）。
// Layout of shaders.bin, the binary shader library packed by the build box (script shbin_gen.py):
// the device reads it through these structs only and never parses JSON.
#pragma once
#include <stdint.h>

#define SHB_MAGIC 0x534b5644u      // 'DVKS'
#define SHB_VER   1

typedef struct {
    uint32_t magic, ver;
    uint32_t nentries, nbinds, nvis;
    uint32_t c1_total, spv_total, reserved0;
} shb_hdr;                                   /* 32 */

// 一个着色器段（键 = 控制段 0x880 字节的 sha256 前 8 字节）
// One shader stage blob; key = the first 8 bytes of sha256 of its 0x880-byte control segment
typedef struct {
    uint8_t key[8];
    uint8_t stage;                   // 0 顶点 1 像素 2 计算 | 0 vertex, 1 fragment, 2 compute
    uint8_t nb;                      // 绑定条数 | binding count
    uint8_t nvi;                     // 顶点输入条数（只有顶点段有）| vertex input count (vertex stages only)
    uint8_t flags;                   // bit0：有 local（派发尺寸）| bit0: has local_size
    uint8_t local[4];                // x, y, z（第 4 字节补零对齐）| x, y, z (4th byte pads to align)
    uint32_t bind_off, vi_off;       // 下标进 shb_bind / shb_vi 数组 | indices into the shb_bind / shb_vi arrays
    uint32_t spv_off, spv_words;     // SPIR-V 块里的字偏移 / 字数 | word offset / word count inside the SPIR-V blob
    uint32_t c1_off, c1_len;         // c1 常量段在 c1 块里的偏移 / 长度（0 = 没有）| offset / length inside the c1 blob (0 = none)
} shb_entry;                                   /* 40 */

typedef struct {
    uint8_t binding;                 // SPIR-V 里的 binding 号（set 恒 0）| the SPIR-V binding number (set is always 0)
    uint8_t kind;                    // 0 ubo 1 combined 2 ssbo 3 image
    uint8_t nvn_kind;                // 0 support 1 cbuf 2 c1 3 drvcb 4 samp 5 samp_tex 6 sbuf 7 ubo_ptr 8 img 9 tbuf
    uint8_t bank;                    // combined：贴图寄存器 t + 1（nvn_reg 放的是采样器 s；0 = 旧库没带 t）；drvcb：驱动段号（0 或 2）
                                     // combined: the texture register t plus one (nvn_reg holds the sampler s; 0 = an old library carries no t); drvcb: which driver segment (0 or 2)| combined: the texture register t (nvn_reg holds the sampler s); drvcb: which driver segment (0 or 2)
    int16_t nvn_reg;                 // NVN 槽号；-1 = 没有（support/c1/drvcb）| NVN slot number; -1 when absent
    uint8_t rw;                      // ssbo / image 是否写 | whether the ssbo / image writes
    uint8_t vtype;                   // 采样器维数（低 4 位：0 2D 1 2DArray 2 3D 3 Cube 4 Buffer）+ bit4 shadow
} shb_bind;                                    /* 8 */

typedef struct { uint8_t loc, cls, pad0, pad1; } shb_vi;   // cls：0 浮点 1 uint 2 sint | cls: 0 float, 1 uint, 2 sint

_Static_assert(sizeof(shb_hdr) == 32, "shb_hdr 必须 32 字节");
_Static_assert(sizeof(shb_entry) == 40, "shb_entry 必须 40 字节");
_Static_assert(sizeof(shb_bind) == 8, "shb_bind 必须 8 字节");

enum { SHB_UBO = 0, SHB_COMBINED = 1, SHB_SSBO = 2, SHB_IMAGE = 3 };
enum { SHB_N_SUPPORT = 0, SHB_N_CBUF, SHB_N_C1, SHB_N_DRVCB, SHB_N_SAMP, SHB_N_SAMPT,
       SHB_N_SBUF, SHB_N_UBOP, SHB_N_IMG, SHB_N_TBUF };
enum { SHB_DIM_2D = 0, SHB_DIM_2DARRAY, SHB_DIM_3D, SHB_DIM_CUBE, SHB_DIM_BUFFER };
