// OHOS 手柄 / 触屏输入进宿主的那一面：napi 注册 + 按键 / 摇杆 / 触点三个入口 + 一个诊断串。
// The OHOS side of controller / touch input into the host: napi registration, the key / stick / touch entries and one diagnostic string.
#pragma once
#include <stdint.h>
#include "napi/native_api.h"

// app_ohos.c 的模块注册里调（与 xcomp_attach 同一模子）：挂 padKey / padAxis / padTouch / padDiag 到 exports
// Called from the module registration in app_ohos.c (same shape as xcomp_attach): attaches padKey / padAxis / padTouch / padDiag to exports
int pad_ohos_register(napi_env env, napi_value exports);
// 外部开关落定后（app_ohos.c 的 Start 里）调一次：DM_PAD=0 把整个手柄面关掉，用来和纯图形轮做 A/B
// Call once after the external switches land (in app_ohos.c's Start): DM_PAD=0 shuts the whole controller face off, for an A/B against graphics-only rounds
void pad_ohos_env(void);
// code = HarmonyOS KeyCode（十字键 2012–2016、手柄键 2301–2315），type = ArkTS KeyType（1=抬起，其余算按下）；这个键归手柄就返回 1
// code = HarmonyOS KeyCode (D-pad 2012-2016, gamepad buttons 2301-2315), type = ArkTS KeyType (1 = up, anything else counts as pressed); returns 1 when the key belongs to the controller
int pad_ohos_key(int code, int type);
// stick 0 = 左摇杆、1 = 右摇杆，x / y 取 [-1,1] 且上为正（±32767 换算在本文件做） | stick 0 = left, 1 = right, x / y in [-1,1] with up positive (the +/-32767 scaling happens in this file)
void pad_ohos_axis(int stick, double x, double y);
// 屏幕虚拟手柄的一颗键：gp 编号沿用星露谷工程 PadLayout.ets 的约定（0-3=A/B/X/Y、4=－、6=＋、7/8=LS/RS、9/10=L/R、100/101=ZL/ZR、300-303=十字上下左右），
// 直接给游戏键位、不参与实体手柄的键帽换算；down 非 0 = 按下。这个号认识就返回 1
// One button of the on-screen pad: gp numbering follows the Stardew project's PadLayout.ets (0-3=A/B/X/Y, 4=minus, 6=plus, 7/8=LS/RS, 9/10=L/R, 100/101=ZL/ZR, 300-303=D-pad up/down/left/right),
// given straight as game buttons with no glyph conversion for physical controllers; down != 0 means pressed. Returns 1 when the number is known
int pad_ohos_btn(int gp, int down);
// 触屏当十字键：action 0 按下 / 1 抬起 / 2 移动 / 3 取消，(x, y) 触点、(w, h) 组件尺寸，同一单位即可
// Touch as D-pad: action 0 down / 1 up / 2 move / 3 cancel, (x, y) the point, (w, h) the component size, same units are enough
void pad_ohos_touch(int action, float x, float y, float w, float h);
// 一行诊断（不含百分号）：本端发出的位掩码 vs 游戏这次取手柄取到的位掩码；LOG("%s", pad_ohos_diag()) 直接打
// One diagnostic line (contains no percent sign): the bitmask this side published vs. the bitmask the game actually got on its controller read; log it with LOG("%s", pad_ohos_diag())
const char *pad_ohos_diag(void);
