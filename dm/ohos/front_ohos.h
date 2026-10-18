// OHOS 前端对外那两面：软键盘的 napi 注册（页面用 kbdPoll / kbdAnswer 这一来一回）。
// The OHOS front end's outward face: the software keyboard's napi registration (the page's kbdPoll / kbdAnswer round trip).
#pragma once
#include "napi/native_api.h"

// app_ohos.c 的模块注册里调（与 pad_ohos_register 同一模子）：挂 kbdPoll / kbdAnswer 到 exports
// Called from the module registration in app_ohos.c (same shape as pad_ohos_register): attaches kbdPoll / kbdAnswer to exports
int front_ohos_register(napi_env env, napi_value exports);
