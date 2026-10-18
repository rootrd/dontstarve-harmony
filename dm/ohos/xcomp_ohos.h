// XComponent 接线（M2 起点）：从 NAPI exports 解出 OH_NativeXComponent，注册表面回调，
// 把回调里的 window（OHNativeWindow*）+ 尺寸交给 vk_smoke；触摸/键鼠后续挂这里。
#pragma once
#include "napi/native_api.h"

// 在模块注册函数里调用：exports 带 __NATIVE_XCOMPONENT_OBJ__ 时接线，返回 1 = 接上了
int xcomp_attach(napi_env env, napi_value exports);

// 当前表面（OnSurfaceCreated 之后、OnSurfaceDestroyed 之前才有）：win = OHNativeWindow*，
// w/h = 表面像素尺寸。没有表面返回 0。Vulkan 重放层（nvn_vulkan.c）用这条拿窗口。
int dm_ohos_window(void **win, uint64_t *w, uint64_t *h);
