// M2-0 通条冒烟对外接口 | Public hooks of the M2-0 Vulkan smoke path
#pragma once
#include <stdint.h>

// 表面就绪（window = OHNativeWindow*，来自 XComponent 回调）
void vk_smoke_set_window(void *window, uint64_t w, uint64_t h);
// DM_VK_SMOKE=1 且表面已就绪时开一条冒烟线程（幂等）
void vk_smoke_maybe_start(void);
// 表面销毁：让冒烟线程停下并拆掉 swapchain
void vk_smoke_window_gone(void);
