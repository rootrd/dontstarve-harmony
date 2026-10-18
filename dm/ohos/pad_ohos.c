// 手柄 / 触屏 → 宿主：页面 ArkTS 的按键与触点 → napi（padKey / padAxis / padTouch）→ 本文件 → dm_pad_set（hle_hid.c，游戏取手柄的那一处）。
// 走 napi 不走原生输入监听：OH_Input_AddMonitor / AddKeyEventHook 要 INPUT_MONITORING、HOOK_KEY_EVENT 这类系统权限，普通 HAP 拿不到；
// OH_NativeXComponent_RegisterKeyEventCallback 只在 XComponent 拿到焦点时来事件，而页面级 onKeyEventDispatch 不挑焦点，触点也只能在 ArkTS 侧一起拿到。
// Controller / touch -> host: the page's ArkTS key and touch events -> napi (padKey / padAxis / padTouch) -> this file -> dm_pad_set (hle_hid.c, where the game reads the controller).
// The reason for napi instead of a native input listener: OH_Input_AddMonitor / AddKeyEventHook need system permissions (INPUT_MONITORING, HOOK_KEY_EVENT) that an ordinary HAP cannot get;
// OH_NativeXComponent_RegisterKeyEventCallback only delivers when the XComponent has focus, while the page-level onKeyEventDispatch does not care about focus and also brings the touch points along.
#include "pad_ohos.h"
#include "dm.h"           // LOG：stderr → app_ohos.c 的 hilog 桥（宿主各处用的同一个） | LOG: stderr -> the hilog bridge in app_ohos.c (the same one the host uses everywhere)
#include "dm_input.h"
#include "dm_padmap.h"
#include <stdatomic.h>
#include <string.h>

enum { KC_UP = 2012, KC_DOWN = 2013, KC_LEFT = 2014, KC_RIGHT = 2015, KC_CENTER = 2016 };
enum { KC_A = 2301, KC_B = 2302, KC_X = 2304, KC_Y = 2305, KC_L1 = 2307, KC_R1 = 2308,
       KC_L2 = 2309, KC_R2 = 2310, KC_SELECT = 2311, KC_START = 2312, KC_THUMBL = 2314, KC_THUMBR = 2315 };
// 映射表：face=0 的键直接给游戏键位（Start=＋、Select=－；十字中键 = 确认，有的手柄只有十字 + 中键）；
// face=1 的四个面键先按位置存（下 / 右 / 左 / 上），再由 dm_padmap_face 换成游戏键——键帽字母与手柄型号的对应（Switch Pro / Xbox 排法不同）
// 与 iOS 的 dm_gamepad.m 共用 dm_padmap.h 那一份规则；这里认不出手柄类型，用 DM_PAD_LAYOUT=switch/nintendo 指认，没设就按 Xbox 排（下方键 = 确认）。
// The mapping table: face = 0 keys give the game bit directly (Start = +, Select = -; D-pad center = confirm, some controllers only have a D-pad plus a center button);
// face = 1 is the four face buttons, first stored by position (bottom / right / left / top), then turned into game bits by dm_padmap_face -- the letter on the button glyph versus the controller model (Switch Pro and Xbox are laid out differently)
// shares dm_padmap.h's one set of rules with iOS's dm_gamepad.m; the controller type cannot be recognized here, so DM_PAD_LAYOUT=switch/nintendo states it and with nothing set the Xbox layout is used (bottom button = confirm).
enum { F_SOUTH = 1, F_EAST = 2, F_WEST = 4, F_NORTH = 8 };   // 面键现值（下 / 右 / 左 / 上那颗按着没有） | current face buttons (whether the bottom / right / left / top one is held)
static const struct { int code; uint64_t mask; int face; } k_map[] = {
    { KC_UP, DM_BTN_UP, 0 }, { KC_DOWN, DM_BTN_DOWN, 0 }, { KC_LEFT, DM_BTN_LEFT, 0 }, { KC_RIGHT, DM_BTN_RIGHT, 0 },
    { KC_CENTER, DM_BTN_A, 0 }, { KC_L1, DM_BTN_L, 0 }, { KC_R1, DM_BTN_R, 0 }, { KC_L2, DM_BTN_ZL, 0 }, { KC_R2, DM_BTN_ZR, 0 },
    { KC_START, DM_BTN_PLUS, 0 }, { KC_SELECT, DM_BTN_MINUS, 0 }, { KC_THUMBL, DM_BTN_LS, 0 }, { KC_THUMBR, DM_BTN_RS, 0 },
    { KC_A, F_SOUTH, 1 }, { KC_B, F_EAST, 1 }, { KC_X, F_WEST, 1 }, { KC_Y, F_NORTH, 1 } };
static uint64_t g_face, g_pad;            // 面键位置现值 / 十字、肩、菜单键的游戏位现值（只有 UI / napi 线程写） | the face buttons by position / the D-pad, shoulder and menu game bits (only the UI / napi thread writes them)
static int32_t g_ax[4];                   // 摇杆 ±32767（上为正） | sticks +/-32767 (up positive)
static uint64_t g_tpad;                   // 触屏当十字键给的那几颗 | the D-pad buttons synthesized from touch
static struct { int on; float ox, oy; } g_t;

// ---- 屏幕虚拟手柄（Index.ets 画的按键）----
// 实体手柄的四个面键只能按「位置」收（系统给的 2301~2305 就是下/右/左/上那颗，不看键帽字母），再经 dm_padmap_face 按手柄排法换成游戏键；
// 屏幕上的按键是我们自己画的，字和位都是定的，所以直接指名游戏键，不参与键帽换算 —— 否则 DM_PAD_LAYOUT 一改成 switch，
// 屏幕上那颗写着 A 的键就会发成 B。gp 编号沿用星露谷那份工程（PadLayout.ets）的约定，两边的布局文件可以互看。
// The four face buttons of a physical controller can only be collected by position (the system's 2301-2305 mean the bottom/right/left/top glyph, ignoring the letter), then turned into game keys by dm_padmap_face according to the controller's layout;
// the on-screen buttons are drawn by us, so both the letter and the position are ours to know -- they name the game button directly and take no part in the glyph conversion, otherwise setting DM_PAD_LAYOUT=switch would make the button labelled A send B.
// The gp numbering follows the Stardew project's PadLayout.ets so the two layout files read the same.
static uint64_t g_vpad;
static const struct { int gp; uint64_t mask; } v_map[] = {
    { 0, DM_BTN_A }, { 1, DM_BTN_B }, { 2, DM_BTN_X }, { 3, DM_BTN_Y },
    { 4, DM_BTN_MINUS }, { 6, DM_BTN_PLUS }, { 7, DM_BTN_LS }, { 8, DM_BTN_RS },
    { 9, DM_BTN_L }, { 10, DM_BTN_R }, { 100, DM_BTN_ZL }, { 101, DM_BTN_ZR },
    { 300, DM_BTN_UP }, { 301, DM_BTN_DOWN }, { 302, DM_BTN_LEFT }, { 303, DM_BTN_RIGHT } };

static _Atomic uint64_t g_pub, g_seen;    // 发出的 / 游戏这次取手柄收到的位掩码（在宿主自己的页过滤之前） | the published and the bitmask the controller read received (before the host's own page filters)
static _Atomic int32_t g_seen_lx, g_seen_ly;
void dm_padmode_request(int pad);         // 宿主 dm_padmode.c：布局换成原版手柄版 / 触屏版 | Host dm_padmode.c: switch the layouts to the original controller version / the touch version
int dm_padmode_current(void);             // 宿主 dm_padmode.c：此刻是哪套布局（1 手柄 / 0 触屏） | Host dm_padmode.c: which layout is in effect now (1 controller / 0 touch)

static int pad_touch_on(void) { static int set = -1; if (set < 0) { const char *e = getenv("DM_PAD_TOUCH"); set = !(e && !strcmp(e, "0")); } return set; }

static int g_pad_off;     // DM_PAD=0：整个手柄面（含注入客体那一步）不接，用来和纯图形轮做 A/B

static uint64_t compose(void) {
    if (g_pad_off) return 0;
    uint64_t b = dm_padmap_face(dm_padmap_kind(getenv("DM_PAD_LAYOUT"), NULL), g_face & F_SOUTH, g_face & F_EAST,
                                g_face & F_WEST, g_face & F_NORTH) | g_pad | g_tpad | g_vpad;
    atomic_store(&g_pub, b);
    dm_pad_set(b, g_ax[0], g_ax[1], g_ax[2], g_ax[3]);
    return b;
}

// 屏幕按键按下 / 松开：返回 1 = 这个 gp 号认识（页面据此消费掉这次触摸） | on-screen button press / release: 1 = this gp number is known (the page consumes the touch accordingly)
int pad_ohos_btn(int gp, int down) {
    unsigned i;
    for (i = 0; i < sizeof v_map / sizeof *v_map; i++) if (v_map[i].gp == gp) break;
    if (i == sizeof v_map / sizeof *v_map) return 0;
    if (down) { static int asked; if (!asked) { asked = 1; dm_padmode_request(1); LOG("[pad] 屏幕手柄首次按下 → 手柄布局"); } }
    if (down) g_vpad |= v_map[i].mask; else g_vpad &= ~v_map[i].mask;   // 单独存：实体手柄松开同一颗不该把屏幕上按着的这颗带走 | kept separate: releasing the same button on a physical pad must not clear the on-screen hold
    compose();
    return 1;
}

int pad_ohos_key(int code, int type) {
    int down = type != 1;                 // ArkTS KeyType：1 = Up，其余（按下 / 长按）都算按着 | ArkTS KeyType: 1 = up, anything else (press / long press) counts as held
    unsigned i;
    for (i = 0; i < sizeof k_map / sizeof *k_map; i++) if (k_map[i].code == code) break;
    if (i == sizeof k_map / sizeof *k_map) return 0;   // 键盘 / 音量一类的键不接，交回页面照常处理 | keyboard / volume keys are not taken, handed back to the page as usual
    if (k_map[i].face) { if (down) g_face |= k_map[i].mask; else g_face &= ~k_map[i].mask; }
    else {
        if (down) { static int asked; if (!asked) { asked = 1; dm_padmode_request(1); LOG("手柄[ohos]：见到实体手柄 → 手柄布局"); } }   // 第一次按键时换布局（与 iOS 同） | the layout is switched on the first key press (same as iOS)
        if (down) g_pad |= k_map[i].mask; else g_pad &= ~k_map[i].mask;
    }
    compose();
    return 1;
}

static int32_t axis_raw(double v) {
    if (v > 1) v = 1; if (v < -1) v = -1;
    if (v > -0.15 && v < 0.15) v = 0;     // 死区：静置的摇杆不该给游戏非零值 | dead zone: an idle stick must not hand the game a non-zero value
    return (int32_t)(v * DM_STICK_MAX + (v >= 0 ? 0.5 : -0.5));
}
void pad_ohos_axis(int stick, double x, double y) {
    if (stick < 0 || stick > 1) return;
    g_ax[2 * stick] = axis_raw(x); g_ax[2 * stick + 1] = axis_raw(y);
    compose();
}

void pad_ohos_touch(int action, float x, float y, float w, float h) {
    if (!pad_touch_on()) return;
    uint64_t m = 0;
    if (action == 0) { g_t.on = 1; g_t.ox = x; g_t.oy = y; }
    else if (action == 2 && g_t.on && w > 0 && h > 0) {
        float dx = (x - g_t.ox) / w, dy = (y - g_t.oy) / h;   // 按下点为原点，主方向 = 十字键（8% 死区） | the press point is the origin, the dominant direction = the D-pad key (8% dead zone)
        if (dx * dx + dy * dy > 0.08f * 0.08f)
            m = (dx > dy && dx > -dy) ? DM_BTN_RIGHT : (dx < dy && dx < -dy) ? DM_BTN_LEFT : (dy > dx && dy > -dx) ? DM_BTN_DOWN : DM_BTN_UP;
    } else if (action == 1 || action == 3) g_t.on = 0;
    if (m == g_tpad) return;
    g_tpad = m;
    compose();
}

// 取手柄处看一眼游戏这次真正拿到什么（hle_hid.c fill 的注入点；原先只有 Mac 的触点脚本占着，这里只记录不改值）
// One look at what the game actually got on this read (the injection point of hle_hid.c fill; previously only the Mac touch script held it, here it only records and never changes the value)
static void (*g_prev_hook)(uint64_t, uint64_t *, int32_t *, int32_t *, int32_t *, int32_t *);
static _Atomic int g_pad_reg;     // 注册被调了几次：>1 = 同一个 .so 被加载了两遍，钩子会把自己当成前置钩子（B18 真机把取手柄线程钉死在这条尾调用上）
static void pad_observe(uint64_t now, uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (g_pad_off) return;
    static int once;
    if (!once) { once = 1; LOG("[pad] 钩子首次执行：注册=%d 前置=%p 自身=%p", atomic_load(&g_pad_reg), (void *)g_prev_hook, (void *)pad_observe); }
    atomic_store(&g_seen, *b); atomic_store(&g_seen_lx, *lx); atomic_store(&g_seen_ly, *ly);
    if (g_prev_hook) g_prev_hook(now, b, lx, ly, rx, ry);
}
void pad_ohos_env(void) {
    const char *e = getenv("DM_PAD");
    g_pad_off = (e && !strcmp(e, "0"));
    LOG("[pad] 手柄面 %s", g_pad_off ? "关（DM_PAD=0）" : "开");
}
const char *pad_ohos_diag(void) {
    static char buf[160];
    snprintf(buf, sizeof buf, "[pad] ohos 发出=0x%04llx 客体取到=0x%04llx 轴=%d,%d 取手柄=%llu 手柄布局=%d",
             (unsigned long long)atomic_load(&g_pub), (unsigned long long)atomic_load(&g_seen),
             (int)atomic_load(&g_seen_lx), (int)atomic_load(&g_seen_ly),
             (unsigned long long)dm_input_samples(), dm_padmode_current());
    return buf;
}

// ---- napi 面 ----
static napi_value Int(napi_env env, long v) { napi_value r = NULL; napi_create_int64(env, v, &r); return r; }
static napi_value PadKey(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value a[2] = {NULL, NULL}; int32_t code = 0, type = 0;
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    if (argc < 1) return Int(env, 0);
    napi_get_value_int32(env, a[0], &code);
    if (argc > 1) napi_get_value_int32(env, a[1], &type);
    return Int(env, pad_ohos_key(code, type));
}
static napi_value PadAxis(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value a[3] = {NULL, NULL, NULL}; int32_t stick = 0; double x = 0, y = 0;
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    if (argc < 3) return NULL;
    napi_get_value_int32(env, a[0], &stick); napi_get_value_double(env, a[1], &x); napi_get_value_double(env, a[2], &y);
    pad_ohos_axis(stick, x, y);
    return NULL;
}
static napi_value PadTouch(napi_env env, napi_callback_info info) {
    size_t argc = 5; napi_value a[5]; int32_t act = 0; double v[4] = {0, 0, 0, 0};
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    if (argc < 5) return NULL;
    napi_get_value_int32(env, a[0], &act);
    for (int i = 0; i < 4; i++) napi_get_value_double(env, a[i + 1], &v[i]);
    pad_ohos_touch(act, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
    return NULL;
}
static napi_value PadBtn(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value a[2] = {NULL, NULL}; int32_t gp = 0, down = 0;
    napi_get_cb_info(env, info, &argc, a, NULL, NULL);
    if (argc < 2) return Int(env, 0);
    napi_get_value_int32(env, a[0], &gp); napi_get_value_int32(env, a[1], &down);
    return Int(env, pad_ohos_btn(gp, down));
}
static napi_value PadDiag(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value r = NULL;
    napi_create_string_utf8(env, pad_ohos_diag(), NAPI_AUTO_LENGTH, &r);
    return r;
}

int pad_ohos_register(napi_env env, napi_value exports) {
    static napi_property_descriptor desc[] = {
        {"padKey", NULL, PadKey, NULL, NULL, NULL, napi_default, NULL},
        {"padAxis", NULL, PadAxis, NULL, NULL, NULL, napi_default, NULL},
        {"padTouch", NULL, PadTouch, NULL, NULL, NULL, napi_default, NULL},
        {"padBtn", NULL, PadBtn, NULL, NULL, NULL, napi_default, NULL},
        {"padDiag", NULL, PadDiag, NULL, NULL, NULL, napi_default, NULL},
    };
    // 接在已有钩子前面（OHOS 上目前是空）：诊断要看的是游戏那一次真正拿到的值 | chained in front of an existing hook (currently empty on OHOS): the diagnostic must show what that read really got
    if (dm_pad_hook != pad_observe) { g_prev_hook = dm_pad_hook; dm_pad_hook = pad_observe; }   // 已经接过就不再把自己链到自己前面
    atomic_fetch_add(&g_pad_reg, 1);
    if (!exports) return 0;
    LOG("[pad] ohos 手柄面已接：padKey/padAxis/padTouch/padBtn/padDiag");
    return napi_define_properties(env, exports, sizeof desc / sizeof desc[0], desc) == napi_ok ? 1 : 0;
}
