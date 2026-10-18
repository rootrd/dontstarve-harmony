// nn::ro：Horizon 的重定位模块（NRO）服务。宿主没有 ro: 服务进程，SDK 客户端的静态对象也没人初始化：
// 实测 nnMain 调 ro::InitializeWithPortName，崩在 pc=sdk+0x13bc80 的 `str x9,[x8]`，x8 = *(u64*)*(sdk+0xd4a858) = 0
// → SIGSEGV@0 被系统信号处理器带走（退出码 139；现场不在 hilog，在 $DM_SAVE/crash_<pid>.txt）。
// nnMain 的这一步是任天堂的启动样板；游戏本体是否真要装 NRO 未知，所以 init/注销类空接报成功，
// 要真模块语义的几条如实回通用失败，并把现场（镜像头 16 字节、大小、符号名）打出来，
// 下一轮据此决定要不要在宿主补一个真 NRO 装载器。
// nn::ro: Horizon's relocatable-module (NRO) service. The host has no ro: service process, and nobody initializes the SDK
// client's statics: measured, nnMain calls ro::InitializeWithPortName and dies at pc=sdk+0x13bc80 on `str x9,[x8]` with
// x8 = *(u64*)*(sdk+0xd4a858) = 0 -> SIGSEGV@0 taken by the system handler (exit code 139; the scene lands in
// $DM_SAVE/crash_<pid>.txt, not in hilog). That nnMain step is Nintendo's boot boilerplate, and whether the title really
// loads an NRO is unknown, so the init/unregister entries are accepted as no-ops while the ones needing real module
// semantics return a generic failure and print the scene (image header, size, symbol name) -- the evidence for deciding
// whether the host grows a real NRO loader.
#include "hle.h"

HLE_FORCE("_ZN2nn2ro10InitializeEv", "ro: 服务；ro 客户端静态对象宿主没有，空接报成功", Result, h_ro_Init, void) { LOG("ro 接管：Initialize（空接）"); return RES_OK; }
HLE_FORCE("_ZN2nn2ro22InitializeWithPortNameEPKc", "ro: 服务；同上，端口名只打印", Result, h_ro_InitPort, const char *name) { LOG("ro 接管：InitializeWithPortName(%s)（空接）", name ? name : "(空)"); return RES_OK; }
HLE_FORCE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", "ro: 服务；没装载过模块，卸载空接", Result, h_ro_Unload, void *mod) { (void)mod; return RES_OK; }
HLE_FORCE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", "ro: 服务；模块注册表宿主用不上", Result, h_ro_Reg, void *info, const void *addr) { (void)info; (void)addr; return RES_OK; }
HLE_FORCE("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", "ro: 服务；同上", Result, h_ro_Unreg, void *info) { (void)info; return RES_OK; }

// 下面四条要真模块语义：如实回失败，现场打全（头 16 字节是不是 NRO 的 "NERo" 魔数、多大、要哪个符号）。
// The four below need real module semantics: fail honestly, print the whole scene (is the header the NRO "NERo" magic,
// how large, which symbol).
static void ro_scene(const char *what, const void *img, size_t sz) {
    char hex[40] = "";
    if (img) { const uint8_t *p = (const uint8_t *)img; for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", p[i]); }
    LOG("ro %s：镜像=%p 大小=%#zx 头16=%s → 报失败", what, img, sz, hex);
}
HLE_FORCE("_ZN2nn2ro13GetBufferSizeEPmPKv", "ro: 服务；宿主暂无 NRO 装载器", Result, h_ro_Size, size_t *out, const void *img) {
    ro_scene("GetBufferSize", img, 0); if (out) *out = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", "ro: 服务；宿主暂无 NRO 装载器", Result, h_ro_Load, void *mod, const void *img, void *scratch, size_t sz, int slot) {
    (void)mod; (void)scratch; (void)slot; ro_scene("LoadModule", img, sz); return RES_FAIL;
}
HLE_FORCE("_ZN2nn2ro12LookupSymbolEPmPKc", "ro: 服务；没有已装载模块可查", Result, h_ro_Lookup, size_t *out, const char *name) {
    (void)out; LOG("ro LookupSymbol(%s) → 报失败", name ? name : "(空)"); return RES_FAIL;
}
HLE_FORCE("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", "ro: 服务；同上", Result, h_ro_LookupMod, size_t *out, const void *mod, const char *name) {
    (void)out; (void)mod; LOG("ro LookupModuleSymbol(%s) → 报失败", name ? name : "(空)"); return RES_FAIL;
}
