// OHOS 前端：front.h 的三面（跑客体 / 出图分辨率 / 文本输入）+ 软键盘的 napi 接缝。
// 文本输入这条路：客体线程（hle_swkbd.c 的 swkbd_show）把请求挂在这儿，页面 ArkTS 每 100 ms 轮一次 kbdPoll 取走，
// 弹一个带 TextInput 的对话框（系统输入法由此而来），确定 / 取消再经 kbdAnswer 回填、唤醒客体线程。
// 为什么走轮询而不是 napi_threadsafe_function：这条路上没有需要立刻送达的事件，100 ms 的延迟对「打字」完全无感，
// 换来的是不用管 env 生命周期与线程安全回调的析构顺序（页面销毁后没人轮，这里靠超时兜底）。
// OHOS front end: front.h's three faces (run the guest / output resolution / text input) plus the software keyboard's napi seam.
// The text-input path: the guest thread (swkbd_show in hle_swkbd.c) parks its request here, the ArkTS page polls kbdPoll every 100 ms and takes it,
// shows a dialog with a TextInput (that is where the system IME comes from), and hands the result back through kbdAnswer, waking the guest thread.
// Why polling instead of napi_threadsafe_function: nothing here needs instant delivery, 100 ms is imperceptible while typing, and it avoids
// env-lifetime and threadsafe-callback teardown ordering (if the page is gone nobody polls - the timeout below covers that).
#include "front.h"
#include "dm.h"           // LOG：stderr → app_ohos.c 的 hilog 桥 | LOG: stderr -> the hilog bridge in app_ohos.c
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "napi/native_api.h"

void front_run(void (*guest)(void)) {
    guest();   // 无窗口：在当前线程直接跑（NAPI 侧自会安排线程） | no window: run on the current thread (the NAPI side arranges threads)
}
void *front_metal_layer(void) { return NULL; }   // 无窗口：NVN 空设备路径 | no window: the NVN null-device path
uint64_t front_game_res(int docked) { return docked ? ((uint64_t)900 << 32) | 1600 : ((uint64_t)720 << 32) | 1280; }

enum { KBD_IDLE = 0, KBD_WAIT, KBD_DONE };
static pthread_mutex_t g_kbd_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_kbd_c = PTHREAD_COND_INITIALIZER;
static int g_kbd_state, g_kbd_ok, g_kbd_ui;      // g_kbd_ui = 页面至少轮过一次（没轮说明没有 UI，别把客体线程钉在等待里） | the page polled at least once: no poll means no UI, so do not pin the guest thread in a wait
static char g_kbd_req[1536];                     // 给前端的请求行：初始文本 \t 标题 \t 副标题 \t 提示 \t 上限 | request line for the page: initial \t title \t sub \t guide \t max
static char g_kbd_ans[3072];

int front_text_input(const char *title, const char *sub, const char *guide, const char *initial,
                     int max_chars, char *out, int out_cap) {
    if (!out || out_cap < 2) return -1;
    pthread_mutex_lock(&g_kbd_m);
    if (!g_kbd_ui) {                       // 前端还没起来（页面没轮询）：按 front.h 的「无窗口」回 -1，调用方自己按取消走
        pthread_mutex_unlock(&g_kbd_m);    // The front end is not up yet (no poll): answer -1 per front.h's "no window" and let the caller treat it as a cancel
        LOG("swkbd 前端：页面还没开始轮询（kbdPoll 一次没来过）→ 按无窗口处理");
        return -1;
    }
    if (g_kbd_state != KBD_IDLE) {         // 上一个输入框还挂着：不叠窗，直接按无窗口
        pthread_mutex_unlock(&g_kbd_m);
        LOG("swkbd 前端：上一个输入框还没交回 → 本次按无窗口处理");
        return -1;
    }
    snprintf(g_kbd_req, sizeof g_kbd_req, "%s\t%s\t%s\t%s\t%d", initial ? initial : "", title ? title : "",
             sub ? sub : "", guide ? guide : "", max_chars);
    g_kbd_ans[0] = 0; g_kbd_ok = 0; g_kbd_state = KBD_WAIT;
    // 等到页面回填。120 s 上限：真在打字的人不会被掐断，页面万一没了也不会把客体线程永久钉住（届时时钟是冻着的）
    // Wait for the page to hand the text back. The 120 s cap means a real typist is not cut off, while a vanished page cannot pin the guest thread forever (the clock is frozen meanwhile)
    for (int left = 1200; g_kbd_state == KBD_WAIT && left > 0; left--) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000 * 1000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_kbd_c, &g_kbd_m, &ts);
    }
    int timed = g_kbd_state == KBD_WAIT, ok = g_kbd_ok;
    char text[sizeof g_kbd_ans];
    snprintf(text, sizeof text, "%s", g_kbd_ans);
    g_kbd_state = KBD_IDLE;
    pthread_mutex_unlock(&g_kbd_m);
    if (timed) { LOG("swkbd 前端：120 s 没人回填 → 按取消"); return 0; }
    if (!ok) return 0;
    snprintf(out, out_cap, "%s", text);
    return 1;
}

// ---- napi 面（页面每 100 ms 轮一次；确定 / 取消回填）----
static napi_value Str(napi_env env, const char *s) { napi_value r = NULL; napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &r); return r; }

static napi_value KbdPoll(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value r;
    pthread_mutex_lock(&g_kbd_m);
    g_kbd_ui = 1;
    const char *s = g_kbd_state == KBD_WAIT ? g_kbd_req : "";
    pthread_mutex_unlock(&g_kbd_m);
    napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &r);
    return r;
}
static napi_value KbdAnswer(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value a[2] = {NULL, NULL}; int32_t ok = 0; char text[3072]; size_t n = 0;
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    if (argc < 2) return Str(env, "bad");
    napi_get_value_string_utf8(env, a[0], text, sizeof text, &n);
    napi_get_value_int32(env, a[1], &ok);
    pthread_mutex_lock(&g_kbd_m);
    snprintf(g_kbd_ans, sizeof g_kbd_ans, "%s", text);
    g_kbd_ok = ok != 0;
    if (g_kbd_state == KBD_WAIT) g_kbd_state = KBD_DONE;
    pthread_cond_broadcast(&g_kbd_c);
    pthread_mutex_unlock(&g_kbd_m);
    return Str(env, "ok");
}
int front_ohos_register(napi_env env, napi_value exports) {
    static napi_property_descriptor desc[] = {
        {"kbdPoll", NULL, KbdPoll, NULL, NULL, NULL, napi_default, NULL},
        {"kbdAnswer", NULL, KbdAnswer, NULL, NULL, NULL, napi_default, NULL},
    };
    if (!exports) return 0;
    LOG("[kbd] 软键盘前端已接：kbdPoll / kbdAnswer");
    return napi_define_properties(env, exports, sizeof desc / sizeof desc[0], desc) == napi_ok ? 1 : 0;
}
