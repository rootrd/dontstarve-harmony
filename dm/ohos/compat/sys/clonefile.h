// OHOS 兼容替身：APFS clonefile / renamex_np → 递归复制 / renameat2(RENAME_EXCHANGE)
// OHOS compat shim: APFS clonefile / renamex_np -> recursive copy / renameat2(RENAME_EXCHANGE)
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int oc_clonefile(const char *src, const char *dst, int flags);
int oc_renamex_np(const char *from, const char *to, unsigned int flags);
#ifdef __cplusplus
}
#endif
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x00000002u
#endif
#ifndef CLONE_NOFOLLOW
#define CLONE_NOFOLLOW 0x0001
#endif
#define clonefile oc_clonefile
#define renamex_np oc_renamex_np
