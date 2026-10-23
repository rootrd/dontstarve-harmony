/* 设备端 BC3 → ASTC 4x4 转码（B 路）。见 nvn_rt_astc.c 的头注释。 */
#ifndef NVN_RT_ASTC_H
#define NVN_RT_ASTC_H

#include <stddef.h>
#include <stdint.h>

/* 建（或按新档位重建）编码器上下文。返回 0 = 可用，非 0 = 错误码字符串见 rt_astc_err()。 */
int rt_astc_init(float preset);
const char *rt_astc_err(void);

/* 把 w×h 的 RGBA8（行距必须是 w*4，紧排）编成 ceil(w/4)*ceil(h/4) 个 ASTC 4x4 块写进 out。
   out_cap 必须 >= 块数*16。返回 0 = 成功。 */
int rt_astc_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out, size_t out_cap);

/* 一步到位：src 是一段按块紧排的 BC3（src_pitch = 每块行字节数，通常 = ceil(w/4)*16），解成 RGBA8 再编成
   ASTC 4x4 块写进 out。两条上传路（显式 B2T 与打包上传）都只调这一个函数，转码口径不会走岔。
   NVN_BC3 是 NVN 的 BC3 格式号，宿主与解码器两边都按它认。 */
#define NVN_BC3 0x44u
int rt_bc3_to_astc(const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, uint8_t *out, size_t out_cap);

/* 记账：张数 / 块数 / 累计纳秒 / 失败数。取参数就靠这四个。 */
void rt_astc_count(uint64_t blocks, uint64_t ns, int fail);
void rt_astc_stat(uint64_t *tex, uint64_t *blocks, uint64_t *ms, uint64_t *fail);

/* 落盘缓存的账：命中（读盘没编）/ 新写 / 盘上出错 / 额度用尽跳过 / 已写字节(MB)。
   任一参数可传 NULL。开关是 DM_RT_ASTC_DIR=<绝对目录>，可选 DM_RT_ASTC_MAX=<额度 MB>。 */
void rt_astc_cache_stat(uint64_t *hit, uint64_t *wrote, uint64_t *io, uint64_t *capped, uint64_t *mb);
const char *rt_astc_cache_dir(void);    // 已开：返回目录；没开：NULL，原因见 rt_astc_cache_off()
const char *rt_astc_cache_off(void);

#endif
