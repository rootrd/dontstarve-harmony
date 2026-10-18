// OHOS 替身：Mach-O 段读取 → ELF 段（链接器 __start_/__stop_ 符号）；只支持本工程用到的三个段名
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
extern const char __start_swhle[], __stop_swhle[];
extern const char __start_swforce[], __stop_swforce[];
extern const char __start_swnvn[], __stop_swnvn[];
extern const char _mh_execute_header;   // 占位：调用处 &_mh_execute_header 传参用
#ifdef __cplusplus
}
#endif
static inline const uint8_t *getsectiondata(const void *hdr, const char *seg, const char *sect, unsigned long *size) {
    (void)hdr; (void)seg;
    const char *a = 0, *b = 0;
    if (!__builtin_strcmp(sect, "swhle")) a = __start_swhle, b = __stop_swhle;
    else if (!__builtin_strcmp(sect, "swforce")) a = __start_swforce, b = __stop_swforce;
    else if (!__builtin_strcmp(sect, "swnvn")) a = __start_swnvn, b = __stop_swnvn;
    if (a && b) { if (size) *size = (unsigned long)(b - a); return (const uint8_t *)a; }
    if (size) *size = 0;
    return 0;
}
