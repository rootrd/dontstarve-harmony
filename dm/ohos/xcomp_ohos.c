// XComponent 接线实现 | XComponent glue: exports → OH_NativeXComponent → surface callbacks
//
// 为什么全用 dlsym：设备的应用 linker namespace 里，libace_ndk.z.so 的
// OH_NativeXComponent_GetXComponentId 对 DT_NEEDED 重定位不可见（实测
// "relocating failed: symbol not found" → 整个 libdmhost.so 载不进来，
// ArkTS 侧拿到 undefined）。dlopen+dlsym 走句柄查表，不受该过滤影响；
// 取不到的函数逐个报出来，M2 要用的触摸/键鼠函数也照这个模子加。
#include "xcomp_ohos.h"
#include "vk_smoke.h"
#include <hilog/log.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "D2R"
#define LOG_DOMAIN 0x3300
#define XLOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

typedef int32_t (*pfn_GetId)(OH_NativeXComponent *, char *, uint64_t *);
typedef int32_t (*pfn_GetSize)(OH_NativeXComponent *, const void *, uint64_t *, uint64_t *);
typedef int32_t (*pfn_Register)(OH_NativeXComponent *, OH_NativeXComponent_Callback *);

static pfn_GetId f_getid;
static pfn_GetSize f_getsize;
static pfn_Register f_regcb;
static OH_NativeXComponent *g_xc;

// 表面现场：回调线程写、宿主线程读（nvn_vulkan.c 靠 dm_ohos_window 拿窗口）
// Surface state: written by the callback thread, read by the host thread (nvn_vulkan.c gets its window through dm_ohos_window)
static volatile uint64_t g_win, g_ww, g_wh;

int dm_ohos_window(void **win, uint64_t *w, uint64_t *h)
{
    uint64_t a = __atomic_load_n(&g_win, __ATOMIC_ACQUIRE);
    if (win) *win = (void *)(uintptr_t)a;
    if (w) *w = __atomic_load_n(&g_ww, __ATOMIC_ACQUIRE);
    if (h) *h = __atomic_load_n(&g_wh, __ATOMIC_ACQUIRE);
    return a != 0;
}

static void resolve_symbols(void)
{
    if (f_regcb) return;
    void *h = dlopen("libace_ndk.z.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = RTLD_DEFAULT;
    f_getid = (pfn_GetId)dlsym(h, "OH_NativeXComponent_GetXComponentId");
    f_getsize = (pfn_GetSize)dlsym(h, "OH_NativeXComponent_GetXComponentSize");
    f_regcb = (pfn_Register)dlsym(h, "OH_NativeXComponent_RegisterCallback");
    XLOG("[xcomp] dlsym：RegisterCallback=%{public}p GetXComponentSize=%{public}p GetXComponentId=%{public}p",
         (void *)f_regcb, (void *)f_getsize, (void *)f_getid);
}

static void OnSurfaceCreated(OH_NativeXComponent *component, void *window)
{
    uint64_t w = 0, h = 0;
    int rc = f_getsize ? f_getsize(component, window, &w, &h) : -1;
    XLOG("[xcomp] OnSurfaceCreated window=%{public}p 尺寸=%{public}u x %{public}u（GetSize rc=%{public}d）",
         window, (unsigned)w, (unsigned)h, rc);
    __atomic_store_n(&g_ww, w, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wh, h, __ATOMIC_RELEASE);
    __atomic_store_n(&g_win, (uint64_t)(uintptr_t)window, __ATOMIC_RELEASE);
    vk_smoke_set_window(window, w, h);
}

static void OnSurfaceChanged(OH_NativeXComponent *component, void *window)
{
    uint64_t w = 0, h = 0;
    if (f_getsize) f_getsize(component, window, &w, &h);
    XLOG("[xcomp] OnSurfaceChanged 尺寸=%{public}u x %{public}u", (unsigned)w, (unsigned)h);
    __atomic_store_n(&g_ww, w, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wh, h, __ATOMIC_RELEASE);
}

static void OnSurfaceDestroyed(OH_NativeXComponent *component, void *window)
{
    (void)component; (void)window;
    XLOG("[xcomp] OnSurfaceDestroyed");
    __atomic_store_n(&g_win, 0, __ATOMIC_RELEASE);
    vk_smoke_window_gone();
}

int xcomp_attach(napi_env env, napi_value exports)
{
    resolve_symbols();
    napi_value obj = NULL;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &obj) != napi_ok || !obj) {
        XLOG("[xcomp] exports 里没有 __NATIVE_XCOMPONENT_OBJ__（本次模块加载不是 XComponent 触发的）");
        return 0;
    }
    napi_valuetype t = napi_undefined;
    napi_typeof(env, obj, &t);
    if (t != napi_object) {
        XLOG("[xcomp] __NATIVE_XCOMPONENT_OBJ__ 类型=%{public}d（不是对象，XComponent 未绑定）", (int)t);
        return 0;
    }
    OH_NativeXComponent *xc = NULL;
    if (napi_unwrap(env, obj, (void **)&xc) != napi_ok || !xc) {
        XLOG("【红】napi_unwrap 取不到 OH_NativeXComponent");
        return 0;
    }
    g_xc = xc;
    static OH_NativeXComponent_Callback cb;
    cb.OnSurfaceCreated = OnSurfaceCreated;
    cb.OnSurfaceChanged = OnSurfaceChanged;
    cb.OnSurfaceDestroyed = OnSurfaceDestroyed;
    if (!f_regcb) { XLOG("【红】OH_NativeXComponent_RegisterCallback 没取到，表面回调接不上"); return 0; }
    int32_t rc = f_regcb(xc, &cb);
    char id[OH_XCOMPONENT_ID_LEN_MAX + 1] = {0};
    uint64_t idlen = sizeof id;
    if (f_getid) f_getid(xc, id, &idlen);
    XLOG("[xcomp] 接线完成 id=%{public}s RegisterCallback rc=%{public}d", id, rc);
    return rc == 0 ? 1 : 0;
}
