// OHOS 兼容替身：CommonCrypto 摘要接口 → 内置公有领域实现（compat/md5.c、compat/sha1.c）
// OHOS compat shim: CommonCrypto digest surface -> vendored public-domain implementations (compat/md5.c, compat/sha1.c)
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t CC_LONG;
#define CC_MD5_DIGEST_LENGTH 16
#define CC_SHA1_DIGEST_LENGTH 20
#ifdef __cplusplus
extern "C" {
#endif
void oc_md5(const void *data, CC_LONG len, unsigned char out[16]);
void oc_sha1(const void *data, CC_LONG len, unsigned char out[20]);
#define CC_SHA256_DIGEST_LENGTH 32
typedef struct { unsigned char _opaque[128]; } CC_SHA256_CTX;   // 内部装 B-Con SHA256_CTX（compat/sha256.h），有静态断言兜底
void CC_SHA256_Init(CC_SHA256_CTX *c);
void CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
void CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
void oc_sha256(const void *data, CC_LONG len, unsigned char out[32]);
#ifdef __cplusplus
}
#endif
#define CC_MD5 oc_md5
#define CC_SHA1 oc_sha1
#define CC_SHA256 oc_sha256
