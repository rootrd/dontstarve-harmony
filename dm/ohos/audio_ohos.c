// OHAudio 出声后端：libohaudio.so 全部 dlopen+dlsym 运行时取（应用 linker namespace 过滤 NDK 直链符号，同 xcomp_ohos.c 的约定）。
// OHAudio output backend: every libohaudio.so symbol is fetched at runtime with dlopen+dlsym (the app linker namespace filters directly linked NDK symbols; same convention as xcomp_ohos.c).
#include "audio_ohos.h"
#include "dm.h"                                  // LOG（dm/host 现有日志宏）| LOG (the existing logging macro in dm/host)
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <ohaudio/native_audiostreambuilder.h>   // 连带 native_audiostream_base.h（回调结构体 / 枚举都在那）| brings native_audiostream_base.h (callback struct / enums)

typedef struct {
    OH_AudioRenderer *r;
    int ch;                                      // 交织声道数，帧 = 2×ch 字节 | interleaved channel count, frame = 2×ch bytes
    void *ctx;
    void (*fill)(void *ctx, uint8_t *dst, uint64_t bytes);
} aoh;

static _Atomic uint64_t g_req_frames, g_del_frames;
static _Atomic int g_last_err;                   // OH_AudioStream_Result 或 -回调错误码 | OH_AudioStream_Result or -callback error code
static const char *g_last_err_where = "无";      // 最后出错的位置（只在错误路径写，读侧不要求原子）| where the last error came from (written only on error paths; the reader needs no atomicity)

static struct {
    OH_AudioStream_Result (*create)(OH_AudioStreamBuilder **, OH_AudioStream_Type);
    OH_AudioStream_Result (*destroy)(OH_AudioStreamBuilder *);
    OH_AudioStream_Result (*srate)(OH_AudioStreamBuilder *, int32_t);
    OH_AudioStream_Result (*chan)(OH_AudioStreamBuilder *, int32_t);
    OH_AudioStream_Result (*fmt)(OH_AudioStreamBuilder *, OH_AudioStream_SampleFormat);
    OH_AudioStream_Result (*lat)(OH_AudioStreamBuilder *, OH_AudioStream_LatencyMode);
    OH_AudioStream_Result (*usage)(OH_AudioStreamBuilder *, OH_AudioStream_Usage);
    OH_AudioStream_Result (*cbsz)(OH_AudioStreamBuilder *, int32_t);
    OH_AudioStream_Result (*setcb)(OH_AudioStreamBuilder *, OH_AudioRenderer_Callbacks, void *);
    OH_AudioStream_Result (*gen)(OH_AudioStreamBuilder *, OH_AudioRenderer **);
    OH_AudioStream_Result (*start)(OH_AudioRenderer *);
    OH_AudioStream_Result (*stop)(OH_AudioRenderer *);
    OH_AudioStream_Result (*release)(OH_AudioRenderer *);
} f;

static void set_err(int code, const char *where) {
    atomic_store_explicit(&g_last_err, code, memory_order_relaxed);
    g_last_err_where = where;
}

// 缺一个必需符号就整个后端不可用（open 返 NULL，hle_audio 自动退静音接收端）。
// If any required symbol is missing the whole backend is unavailable (open returns NULL and hle_audio degrades to the silent sink).
static void resolve_symbols(void) {
    if (f.gen) return;
    void *h = dlopen("libohaudio.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { set_err(-1, "dlopen libohaudio.so"); return; }
    f.create  = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder **, OH_AudioStream_Type))dlsym(h, "OH_AudioStreamBuilder_Create");
    f.destroy = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *))dlsym(h, "OH_AudioStreamBuilder_Destroy");
    f.srate   = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, int32_t))dlsym(h, "OH_AudioStreamBuilder_SetSamplingRate");
    f.chan    = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, int32_t))dlsym(h, "OH_AudioStreamBuilder_SetChannelCount");
    f.fmt     = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, OH_AudioStream_SampleFormat))dlsym(h, "OH_AudioStreamBuilder_SetSampleFormat");
    f.lat     = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, OH_AudioStream_LatencyMode))dlsym(h, "OH_AudioStreamBuilder_SetLatencyMode");
    f.usage   = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, OH_AudioStream_Usage))dlsym(h, "OH_AudioStreamBuilder_SetRendererInfo");
    f.cbsz    = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, int32_t))dlsym(h, "OH_AudioStreamBuilder_SetFrameSizeInCallback");
    f.setcb   = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, OH_AudioRenderer_Callbacks, void *))dlsym(h, "OH_AudioStreamBuilder_SetRendererCallback");
    f.gen     = (OH_AudioStream_Result (*)(OH_AudioStreamBuilder *, OH_AudioRenderer **))dlsym(h, "OH_AudioStreamBuilder_GenerateRenderer");
    f.start   = (OH_AudioStream_Result (*)(OH_AudioRenderer *))dlsym(h, "OH_AudioRenderer_Start");
    f.stop    = (OH_AudioStream_Result (*)(OH_AudioRenderer *))dlsym(h, "OH_AudioRenderer_Stop");
    f.release = (OH_AudioStream_Result (*)(OH_AudioRenderer *))dlsym(h, "OH_AudioRenderer_Release");
    if (!f.create || !f.srate || !f.chan || !f.fmt || !f.setcb || !f.gen || !f.start || !f.stop || !f.release)
        { f.gen = NULL; set_err(-2, "dlsym OHAudio"); }
}

// 写回调（OHAudio 内部线程）：len 字节 PCM 交给上层的 pull；缺口由 pull 补静音，不打断回调。
// Write callback (OHAudio internal thread): hand the len bytes of PCM to the upper layer's pull; gaps are zero-filled by pull, the callback is never blocked.
static int32_t oh_on_write(OH_AudioRenderer *r, void *ud, void *buf, int32_t len) {
    (void)r;
    aoh *a = ud;
    if (len <= 0 || !a->fill) return 0;
    uint64_t frames = (uint64_t)len / (2u * (uint64_t)a->ch);
    atomic_fetch_add_explicit(&g_req_frames, frames, memory_order_relaxed);
    a->fill(a->ctx, (uint8_t *)buf, (uint64_t)len);
    atomic_fetch_add_explicit(&g_del_frames, frames, memory_order_relaxed);
    return len;
}

static int32_t oh_on_event(OH_AudioRenderer *r, void *ud, OH_AudioStream_Event ev) {
    (void)r; (void)ud;
    LOG("audio_ohos 设备事件 %d（继续按当前流播）", (int)ev);   // device event (playback continues on the current stream)
    return 0;
}

static int32_t oh_on_interrupt(OH_AudioRenderer *r, void *ud, OH_AudioInterrupt_ForceType t, OH_AudioInterrupt_Hint hn) {
    (void)r; (void)ud;
    // 打断只记不判：切后台 / 焦点由前端 dm_audio_suspend 统一处理（那边有静音接收端兜节奏）。
    // Interruptions are only recorded: background / focus changes go through the front end's dm_audio_suspend (which keeps the silent sink for pacing).
    set_err(-(1000 + (int)t * 10 + (int)hn), "OHAudio 打断");
    LOG("audio_ohos 打断 type %d hint %d", (int)t, (int)hn);
    return 0;
}

static int32_t oh_on_error(OH_AudioRenderer *r, void *ud, OH_AudioStream_Result err) {
    (void)r; (void)ud;
    set_err((int)err, "OHAudio 错误回调");
    LOG("【红】audio_ohos 错误回调 %d", (int)err);
    return 0;
}

static void *oh_open(int ch, void *ctx, void (*fill)(void *ctx, uint8_t *dst, uint64_t bytes)) {
    resolve_symbols();
    if (!f.gen) { LOG("【红】OHAudio 符号没取到（%s，错误 %d）", g_last_err_where, atomic_load(&g_last_err)); return NULL; }
    aoh *a = calloc(1, sizeof *a);
    a->ch = ch > 0 ? ch : 2; a->ctx = ctx; a->fill = fill;
    OH_AudioStreamBuilder *b = NULL;
    OH_AudioStream_Result rc = f.create(&b, AUDIOSTREAM_TYPE_RENDERER);
    if (rc != AUDIOSTREAM_SUCCESS || !b) { set_err((int)rc, "OH_AudioStreamBuilder_Create"); free(a); return NULL; }
    OH_AudioRenderer_Callbacks cb = { .OH_AudioRenderer_OnWriteData = oh_on_write,
                                      .OH_AudioRenderer_OnStreamEvent = oh_on_event,
                                      .OH_AudioRenderer_OnInterruptEvent = oh_on_interrupt,
                                      .OH_AudioRenderer_OnError = oh_on_error };
    rc = f.srate(b, 48000) | f.chan(b, a->ch) | f.fmt(b, AUDIOSTREAM_SAMPLE_S16LE) | f.setcb(b, cb, a);
    if (f.lat)   f.lat(b, AUDIOSTREAM_LATENCY_MODE_FAST);          // 低延迟模式（可选）| low-latency mode (optional)
    if (f.usage) f.usage(b, AUDIOSTREAM_USAGE_GAME);               // 流的用途（可选）| stream usage (optional)
    if (f.cbsz)  f.cbsz(b, 960);                                   // 一次回调 20 ms @48 kHz（可选）| one callback = 20 ms @48 kHz (optional)
    if (rc != AUDIOSTREAM_SUCCESS) { set_err((int)rc, "OHAudio builder 参数"); f.destroy(b); free(a); return NULL; }
    rc = f.gen(b, &a->r);
    if (f.destroy) f.destroy(b);                                   // 生成后即可销毁构建器 | the builder can be destroyed right after generation
    if (rc != AUDIOSTREAM_SUCCESS || !a->r) { set_err((int)rc, "OH_AudioStreamBuilder_GenerateRenderer"); free(a); return NULL; }
    return a;
}

static int oh_start(void *h) {
    OH_AudioStream_Result rc = f.start(((aoh *)h)->r);
    if (rc != AUDIOSTREAM_SUCCESS) set_err((int)rc, "OH_AudioRenderer_Start");
    return (int)rc;
}
static int oh_stop(void *h) {
    OH_AudioStream_Result rc = f.stop(((aoh *)h)->r);
    if (rc != AUDIOSTREAM_SUCCESS) set_err((int)rc, "OH_AudioRenderer_Stop");
    return (int)rc;
}
static void oh_close(void *h) {
    aoh *a = h;
    f.release(a->r);
    free(a);
}

const dm_audio_backend dm_audio_ohos = { oh_open, oh_start, oh_stop, oh_close };

// 诊断（经 dm/host 的 LOG）：请求帧数 / 实交付帧数 / 最后错误。差值非零 = 有回调没喂满。
// Diagnostic (through LOG from dm/host): frames requested / frames delivered / the last error. A nonzero difference = a callback was not filled.
void dm_audio_ohos_diag(void) {
    LOG("audio_ohos 诊断：请求帧 %llu 交付帧 %llu 最后错误 %d（%s）",
        (unsigned long long)atomic_load_explicit(&g_req_frames, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_del_frames, memory_order_relaxed),
        atomic_load_explicit(&g_last_err, memory_order_relaxed), g_last_err_where);
}
