// nn::vi（显示层）：真机经 vi 服务开显示、建图层、取原生窗口句柄；句柄交给 NVN 的窗口 builder。
// 宿主：返回宿主静态对象；接 Metal 时原生窗口对应前端的 CAMetalLayer（front_metal_layer()）。
// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "显示服务初始化；宿主无 vi 服务", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "开默认显示；宿主给静态对象", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "建图层；宿主给静态对象", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "原生窗口句柄，交给 NVN 窗口 builder；宿主给静态对象", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}

// vsync 事件（HSS/Unity 用；饥荒不走这条）：v10 实测崩在 SDK 自己的 GetDisplayVsyncEvent 里
// pc=sdk+0x3dd4e8 指令=[f94002a8]（ldr x8,[x21]）x21=0，调用者 main+0x60570c。前四条已接管、显示器给了假对象，
// 唯独这条没接管 ⇒ SDK 客户端要连 vi: 服务、拿不到会话就解空引用。
// 接法：在客体线程上用 SDK 自己的 nn::os::CreateSystemEvent 造一个真内核事件（hle_thread.c 的 hle_bind_system_event，
// 与音频缓冲释放事件同一套路），宿主定时线程按 60 Hz 触发。用自动清除：等一次吃掉一次信号 ⇒ 60 Hz 就是 60 fps 上限；
// 手动清除触发一次后永远是已触发态，Unity 的等帧会退化成不限帧空转。
// 触发不挂到出图上：出图在取图处等显示节拍，而 Unity 是先等 vsync 再出图，挂在出图上两边互等。
// Vsync event (used by HSS/Unity; Don't Starve does not take this route). Measured in v10: crash inside the SDK's own
// GetDisplayVsyncEvent, pc=sdk+0x3dd4e8, instruction [f94002a8] (ldr x8,[x21]) with x21=0, caller main+0x60570c. The four entries
// above were already taken over but this one was not, so the SDK client tried to reach the vi: service and dereferenced a null
// session. Fix: create a real kernel event through the SDK's own nn::os::CreateSystemEvent on the guest thread
// (hle_thread.c's hle_bind_system_event, same route as the audio buffer-release event) and signal it from a host timer thread at
// 60 Hz. Auto-clear, because one wait consumes one signal, so 60 Hz caps at 60 fps; manual-clear would stay signaled forever and
// turn Unity's frame wait into an unbounded spin. The signal is not hung on the present path: presenting waits for the display pace
// in acquire while Unity waits for vsync first, so the two would block each other.
#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_vs_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_vs[16];
static int g_nvs, g_vs_started;
static pthread_t g_vs_th;

static void *vi_vs_thread(void *unused) {
    (void)unused;
    struct timespec ts = { 0, 16666667 };       // 60 Hz
    for (;;) {
        nanosleep(&ts, NULL);
        void *snap[16]; int n;
        pthread_mutex_lock(&g_vs_lock); n = g_nvs; memcpy(snap, g_vs, (size_t)n * sizeof snap[0]); pthread_mutex_unlock(&g_vs_lock);
        for (int i = 0; i < n; i++) hle_signal_system_event(snap[i]);
    }
    return NULL;
}

HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "显示器 vsync 事件：造真内核事件、宿主按 60 Hz 触发", Result, h_vi_Vsync, void *ev, void *disp) {
    if (!ev) return RES_FAIL;
    pthread_mutex_lock(&g_vs_lock);
    int seen = 0;
    for (int i = 0; i < g_nvs; i++) if (g_vs[i] == ev) { seen = 1; break; }
    if (!seen) {
        if (g_nvs >= (int)(sizeof g_vs / sizeof g_vs[0])) { pthread_mutex_unlock(&g_vs_lock); LOG("【红】vi vsync 事件超过 %d 个（客体每帧换一个缓冲？）", (int)(sizeof g_vs / sizeof g_vs[0])); return RES_FAIL; }
        g_vs[g_nvs++] = ev;
    }
    int start = !g_vs_started;
    if (start) { g_vs_started = 1; pthread_create(&g_vs_th, NULL, vi_vs_thread, NULL); }
    pthread_mutex_unlock(&g_vs_lock);
    hle_bind_system_event(ev, 1);               // 幂等：绑定表按客体地址去重 | idempotent: the binding table dedups by guest address
    LOG("vi vsync 事件（显示器 %p）→ %p 已绑定", disp, ev);
    return RES_OK;
}
