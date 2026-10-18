// nn::audio AudioOut：游戏（FMOD nx 输出层）按块追加 PCM 缓冲，播完由系统放回「已释放」队列并触发事件。
// 宿主：真出声 = CoreAudio AudioUnit（Mac DefaultOutput / iOS RemoteIO）；静音接收端 = 按真实时间消耗缓冲（不出声，节奏与真机一致）。
//       iOS 缺省真出声（产品必须有声音），测试要静音显式 DM_AUDIO=0；Mac 缺省静音接收端（回放 / 闸门不出声），DM_AUDIO=1 真出声。
// AudioOut / AudioOutBuffer / AudioOutParameter 的内容只经下列接口读写，宿主自定内部布局，只写结构体前部。
// nn::audio AudioOut: the game (FMOD nx output layer) appends PCM buffers block by block; once played the system puts them back on the "released" queue and signals the event.
// Host: real audio output = CoreAudio AudioUnit (Mac DefaultOutput / iOS RemoteIO); silent sink = consumes buffers in real time (no sound, pacing matches the real device).
//       iOS defaults to real audio output (the product must have sound), tests that want silence set DM_AUDIO=0 explicitly; Mac defaults to the silent sink (replay / gate checks are silent), DM_AUDIO=1 for real audio output.
// The contents of AudioOut / AudioOutBuffer / AudioOutParameter are read and written only through the interfaces below; the host defines its own internal layout and only writes the front part of the structs.
#include "hle.h"
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif
#if defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
#include "../ohos/audio_ohos.h"      // 鸿蒙出声后端接缝（函数指针表）| OHOS output backend seam (function-pointer table)
#endif
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // 写进客体 AudioOutBuffer 的前 24 字节 | The first 24 bytes written into the guest AudioOutBuffer
#define QMAX 64
#define RELMAX QMAX                    // 已释放最多 = 在途缓冲数（游戏收回缓冲才能再追加），与待播同上限 | Released max = number of in-flight buffers (the game must take a buffer back before it can append again), same limit as pending
#define ARING (1u << 17)               // 真出声环容量（2 的幂，供掩码）| real-output ring capacity (power of two for the mask)
#define AKEEP(ch) (2880u * (ch))       // 环里最多攒 60 ms（≈3 次回调）：超出由取音端丢掉最旧的一段，延迟不会一路涨 | hold at most 60 ms (~3 callbacks); past that the reader drops the oldest bytes so latency cannot grow
typedef struct {
    pthread_mutex_t m;
    void *event;                       // 客体 SystemEvent：有缓冲被释放就触发 | Guest SystemEvent: signaled whenever a buffer is released
    int ch;
    _Atomic int state;
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // 待播：游戏侧写，出声端读 | Pending: written by the game side, read by the output side
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // 已释放：出声端写，游戏侧读 | Released: written by the output side, read by the game side
    uint64_t pos;                      // 当前块已播字节 | Bytes already played of the current block
#if defined(__APPLE__)
    AudioUnit au;
#else
    void *au;   // OHOS：OHAudio 渲染器句柄（没建成则 NULL，走静音接收端）| OHOS: the OHAudio renderer handle (NULL when it was not built, which goes to the silent sink)
#endif
    pthread_t null_th; _Atomic int null_run;
    // 真出声环（只在建起真渲染器时分配）：宿主 5 ms 拍子往里灌，渲染器回调从里面取。
    // 客体缓冲队列的消耗节奏因此永远由宿主的实时钟顶着 —— 渲染器不起、被系统挂住，也不会把游戏吊死。
    // Real-output ring (allocated only when a real renderer came up): the host's 5 ms tick fills it and the renderer callback drains it.
    // Consuming the guest's buffer queue is paced by the host's real-time clock, so a renderer that never runs cannot hang the game.
    uint8_t *ring; _Atomic uint32_t ring_w, ring_r;
    uint32_t akeep;
    dispatch_semaphore_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
} aout;

// 打开中的输出登记表：前端生命周期（切后台）一次性暂停 / 恢复
// Registry of open outputs: suspended / resumed all at once by the front-end lifecycle (going to background)
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p 不是宿主建的", g); return a; }

static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        dispatch_semaphore_signal(a->notify_sem);
}

static void *event_notify(void *p) {
    aout *a = p;
    for (;;) {
        dispatch_semaphore_wait(a->notify_sem, DISPATCH_TIME_FOREVER);
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            dispatch_semaphore_signal(a->notify_sem);
    }
    return NULL;
}

// 从待播队列取 n 字节交给出声端；整块播完就挪到已释放并通知普通线程。
// Take n bytes from the pending queue and hand them to the output side; once a whole block has played, move it to released and notify the ordinary thread.
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) { memset(dst, 0, n); break; }
        gbuf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut 已释放队列溢出");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
        }
    }
    if (released) notify_release(a);
}

// 真出声环：单写（宿主 5 ms 拍子）单读（声卡回调），单调计数 + 2 的幂掩码，不加锁。
// Real-output ring: one writer (the host's 5 ms tick) and one reader (the sound-card callback), monotonic counters with a power-of-two mask, lock-free.
static void ring_write(aout *a, const uint8_t *src, uint32_t n) {
    uint32_t w = atomic_load_explicit(&a->ring_w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&a->ring_r, memory_order_acquire);
    if (w - r >= ARING) return;                    // 环满：这一拍少灌，取音端自己会丢到 akeep 以下 | ring full: underfill this tick, the reader trims back to akeep
    if (n > ARING - (w - r)) n = ARING - (w - r);
    uint32_t off = w & (ARING - 1), tail = ARING - off;
    memcpy(a->ring + off, src, n < tail ? n : tail);
    if (n > tail) memcpy(a->ring, src + tail, n - tail);
    atomic_store_explicit(&a->ring_w, w + n, memory_order_release);
}

static void ring_read(aout *a, uint8_t *dst, uint32_t n) {
    uint32_t r = atomic_load_explicit(&a->ring_r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&a->ring_w, memory_order_acquire);
    if (w - r > a->akeep) r = w - a->akeep;        // 攒太多就跳到最新：出声可以落后一拍，但不能一直落后
                                                   // Too much queued: jump to the newest bytes; output may slip one tick but must not keep slipping
    uint32_t avail = w - r;
    if (n > avail) { memset(dst + avail, 0, n - avail); n = avail; }   // 取空了补静音（回调不能阻塞）| zero-fill an empty ring (the callback must not block)
    uint32_t off = r & (ARING - 1), tail = ARING - off;
    memcpy(dst, a->ring + off, n < tail ? n : tail);
    if (n > tail) memcpy(dst + tail, a->ring, n - tail);
    atomic_store_explicit(&a->ring_r, r + n, memory_order_release);
}

static _Atomic uint64_t g_render_calls;   // 声卡回调累计（生命周期验证：恢复后须继续增长） | Cumulative sound card callbacks (lifecycle verification: must keep growing after resume)
uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
#if defined(__APPLE__)
static OSStatus au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)f; (void)ts; (void)bus; (void)frames;
    aout *a = ctx;
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed);
    pull(a, bl->mBuffers[0].mData, bl->mBuffers[0].mDataByteSize);
    return noErr;
}
static void au_open(aout *a) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
                                   .componentSubType = kAudioUnitSubType_RemoteIO,
#else
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &a->au)) die("AudioUnit 建不出来");
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 2u * a->ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * a->ch,
                                     .mChannelsPerFrame = (UInt32)a->ch, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {au_render, a};
    if (AudioUnitSetProperty(a->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(a->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(a->au)) die("AudioUnit 设置失败");
}
#elif defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
// OHAudio 写回调线程：只从环里取，环空就补静音 —— 回调里绝不碰客体的待播队列，消耗节奏由宿主拍子管。
// 计数供 dm_audio_render_calls 的生命周期验证。
// The OHAudio write callback: only takes from the ring and zero-fills when it is empty —— it never touches the guest's pending queue, so pacing stays with the host tick.
// The count feeds the lifecycle check of dm_audio_render_calls.
static void ao_fill(void *ctx, uint8_t *dst, uint64_t bytes) {
    aout *a = ctx;
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed);
    if (a->ring) ring_read(a, dst, (uint32_t)bytes);
    else pull(a, dst, bytes);
}
static void au_open(aout *a) {
    if (!(a->au = dm_audio_ohos.open(a->ch, a, ao_fill))) { LOG("【红】OHAudio 渲染器没建出来：本次退静音接收端"); return; }
    a->ring = malloc(ARING);
    if (!a->ring) {   // 环没了回调就只能直接 pull 客体队列（第 112 轮那种卡法）：宁可不出声
        dm_audio_ohos.close(a->au); a->au = NULL; LOG("【红】音频环分配失败：退静音接收端"); return;
    }
    a->akeep = AKEEP(a->ch); if (a->akeep > ARING / 2) a->akeep = ARING / 2;
    atomic_init(&a->ring_w, 0); atomic_init(&a->ring_r, 0);
}
#else
static void au_open(aout *a) { (void)a; die("au_open：OHOS 端口未接 CoreAudio（应走静音接收端 DM_AUDIO=0；产品音频待接 OHAudio）"); }
#endif
// 宿主拍子：每 5 ms 按 48 kHz 实时消耗客体缓冲。静音接收端时它就丢弃；接了真渲染器时它把同一份 PCM 灌进环，
// 由声卡回调取走 —— 客体看到的释放节奏与静音时完全一致，出声只是顺带。
// The host tick: consumes the guest's buffers in real time at 48 kHz every 5 ms. With the silent sink it just discards; with a real renderer it feeds the same PCM into the ring
// for the sound-card callback to take —— the release pacing the guest sees is identical to the silent case, sound output is only a side effect.
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8];
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    for (int beat = 0; atomic_load_explicit(&a->null_run, memory_order_acquire); beat++) {
        t += 5000000; uint64_t now = now_ns(); if (t > now) usleep((useconds_t)((t - now) / 1000));
        pull(a, scratch, bytes);
        if (a->ring) ring_write(a, scratch, (uint32_t)bytes);
        // 每 5 s 一行、最多 5 行：写 / 读 / 声卡回调三个数一起看，就知道渲染器到底有没有在取
        // One line every 5 s, at most five: writer / reader / sound-card callback together show whether the renderer is actually taking anything
        if (a->ring && beat % 1000 == 0 && beat / 1000 < 5)
            LOG("audio 环：写 %u 读 %u 回调 %llu（差 %d 字节）",
                atomic_load_explicit(&a->ring_w, memory_order_relaxed), atomic_load_explicit(&a->ring_r, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&g_render_calls, memory_order_relaxed),
                (int)(atomic_load_explicit(&a->ring_w, memory_order_relaxed) - atomic_load_explicit(&a->ring_r, memory_order_relaxed)));
    }
    return NULL;
}
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); if (e) return atoi(e) != 0;
#if defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
    // 第 112 轮开真渲染器把客体吊死（回调节奏对上客体缓冲队列之前）；现在消耗一律由宿主 5 ms 拍子顶着，
    // 声卡回调只从环里取，取不到补静音 —— 渲染器起不来也只是不出声，不会再卡游戏。DM_AUDIO=0 退回静音接收端。
    // Round 112 hung the guest with a real renderer on (the callback's pace did not match the guest's buffer queue). Consumption is now paced by the host's 5 ms tick
    // and the sound-card callback only takes from the ring, zero-filling when it is empty —— a renderer that cannot start now only means silence, it can no longer stall the game.
    return 1;
#endif
    return TARGET_OS_IPHONE; }

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }，0 = 系统默认（48 kHz / 2 声道）
// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }, 0 = system default (48 kHz / 2 channels)
HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "音频输出参数默认值", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
// AudioOutInfo = { char name[256] }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "音频输出设备列表；宿主 1 个", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "连 audout 服务；宿主 AudioUnit / 静音接收端",
          Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) {
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) die("AudioOut 采样率 %d 不支持（只做 48 kHz）", rate);
    aout *a = calloc(1, sizeof *a);
    pthread_mutex_init(&a->m, NULL); a->event = ev; a->ch = ch;
    atomic_init(&a->state, STATE_STOPPED);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    a->notify_sem = dispatch_semaphore_create(0);
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);   // 缓冲释放事件：客体线程上绑内核事件（自动清除），之后由音频线程触发 | Buffer release event: bind a kernel event on the guest thread (auto-clear), afterwards signaled by the audio thread
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("AudioOut 同时打开超过 %d 个", AOMAX);
    g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    LOG("audio::OpenAudioOut %s 声道 %d %s", name ? name : "(默认)", ch, real_audio() ? "真出声" : "静音接收端");
    if (real_audio()) au_open(a);
    return RES_OK;
}
// 默认音频输出：饥荒（Don't Starve）用的就是这个重载（D2R 用上面那个 4 参的，所以这条不影响 D2R 那版）。
// 不接管时 SDK 会走 nn::audio::OpenAudioOut(3 参) → nn::audio::CreateAudioOutManagerByHipc
//   → nn::sf::hipc::InitializeHipcServiceResolution → nn::sm::Initialize → 连 "sm:" → 宿主没有 sm: 会话 → AbortImpl 自杀。
// 实测 bt: AbortImpl ← VAbortImpl ← sm::Initialize+0x13c ← InitializeHipcServiceResolution ← CreateAudioOutManagerByHipc
//   ← nn::audio::OpenAudioOut ← nn::audio::OpenDefaultAudioOut ← main+0x43c028。
// Default audio output: this is the overload Don't Starve uses (D2R uses the 4-argument one above, so this does not affect it).
// Without a takeover the SDK takes nn::audio::OpenAudioOut(3 args) -> CreateAudioOutManagerByHipc -> InitializeHipcServiceResolution
//   -> nn::sm::Initialize -> connect "sm:" -> no sm: session in the host -> AbortImpl.
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE",
          "默认音频输出（饥荒用的这个重载）：与 OpenAudioOut 同一套路，宿主静音接收端",
          Result, h_au_OpenDefault, void *g, void *ev, const uint8_t *param) { return h_au_Open(g, ev, NULL, param); }
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "开始播放", Result, h_au_Start, void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STARTED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was == STATE_STARTED) return RES_OK;
#if defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
    // 环模式：渲染器只从环里取，喂环的是拍子线程（环的唯一写者）⇒ 两个都要起，不像 macOS 那样二选一。
    // 渲染器起不来只记一行、不再 die —— 第 112 轮把客体吊死的风险就在这条线上。
    // Ring mode: the renderer only takes from the ring and the tick thread is the ring's only writer, so BOTH must run - unlike macOS where they are alternatives.
    // A failed start now only logs; round 112's guest hang came out of exactly this die().
    if (a->au && !g_suspended) { int rc = dm_audio_ohos.start(a->au); if (rc) LOG("【红】OHAudio 启动失败（错误 %d）：本次退静音接收端", rc); }
    atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a);
    return RES_OK;
#else
#if defined(__APPLE__)
    if (a->au && !g_suspended) { if (AudioOutputUnitStart(a->au)) die("AudioUnit 启动失败"); }
    else
#endif
    { atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); }
    return RES_OK;
#endif
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "停止播放", void, h_au_Stop, void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STOPPED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was != STATE_STARTED) return;
    pthread_mutex_lock(&g_aos_lock); int susp = g_suspended; pthread_mutex_unlock(&g_aos_lock);
#if defined(__APPLE__)
    if (a->au && !susp) AudioOutputUnitStop(a->au);
    else
#elif defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
    // 环模式下渲染器和拍子是同时在跑的，两个都得收：漏掉拍子，下次 Start 会再起一条 ⇒ 两条线程同时写一个环
    // In ring mode the renderer and the tick both run, so both have to be taken down here: miss the tick and the next Start spawns a second one - two writers on one ring
    if (a->au && !susp) dm_audio_ohos.stop(a->au);
    if (atomic_load_explicit(&a->null_run, memory_order_acquire))
#endif
    { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); }   // 静音接收端（含暂停期间接手的） | Silent sink (including one that took over during suspension)
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "关闭音频输出", void, h_au_Close, void *g) {
    h_au_Stop(g);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    dispatch_semaphore_signal(a->notify_sem);
    pthread_join(a->notify_th, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
#if defined(__APPLE__)
    if (a->au) { AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au); }
#elif defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
    if (a->au) dm_audio_ohos.close(a->au);
#endif
    dispatch_release(a->notify_sem);
    pthread_mutex_destroy(&a->m); free(a->ring); free(a);
}
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "播放状态", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "采样率", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "声道数", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "采样格式；只做 PCM16", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }

HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "缓冲描述；写进客体结构体前 24 字节", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p; b->cap = cap; b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "缓冲数据指针", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "缓冲数据大小", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "追加待播缓冲", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);
    // 诊断开关 DM_AUDIO_DUMP=<文件>：游戏追加的每块 PCM 原样续写（16 位、48 kHz、声道数见 OpenAudioOut 日志，常见 6）。判「某个音播没播」用：
    // 拿原版音频对它做互相关（一档掉落音就这么验的）。只在设了变量时打开文件，没设时这里只多一次已缓存的判断
    // Diagnostic switch DM_AUDIO_DUMP=<file>: every PCM block the game appends is written out as is (16-bit, 48 kHz, channel count per the OpenAudioOut log, usually 6). Used to judge "did a given sound play or not":
    // cross-correlate the original game's audio against it (this is how the tier-one drop sound was verified). The file is opened only when the variable is set; when unset this costs only one extra cached check
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】DM_AUDIO_DUMP 打不开 %s", e); }
      if (df && b && b->ptr && b->size) fwrite(b->ptr, 1, (size_t)b->size, df); }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool：队列满时 false（真机同样有上限） | bool: false when the queue is full (the real device has a limit too)
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "取已播完的缓冲；没有则 NULL", gbuf *, h_au_Released, void *g) {
    aout *a = ao(g); gbuf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// 前端生命周期 / 音频会话打断：暂停真出声，期间由静音接收端接手按实时消耗（节奏不断）；恢复时切回 AudioUnit。
// 前端在 切后台 / 打断开始 时调 1，回前台 / 打断结束（shouldResume）时调 0。
// Front-end lifecycle / audio session interruption: suspend real audio output, with the silent sink taking over real-time consumption meanwhile (pacing unbroken); switch back to the AudioUnit on resume.
// The front end calls with 1 on going to background / interruption began, and with 0 on returning to foreground / interruption ended (shouldResume).
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on; int failed = 0;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a || !a->au || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) continue;
            // 暂停期间改由静音接收端按实时消耗：否则缓冲不再释放，FMOD 输出线程永久等待、游戏音频节奏卡死（来电打断同理）
            // While suspended the silent sink consumes in real time instead: otherwise buffers are no longer released, the FMOD output thread waits forever and the game's audio pacing stalls (same for an incoming call interruption)
            if (on) {
#if defined(__APPLE__)
                AudioOutputUnitStop(a->au);
#elif defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
                dm_audio_ohos.stop(a->au);
#endif
                // 有环（真出声）时拍子线程本来就没停，暂停期间它继续顶消耗；这里再建一条就成了两个写者
                if (!a->ring) { atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); } }
            else { if (!a->ring) { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); }
#if defined(__APPLE__)
                   OSStatus st = AudioOutputUnitStart(a->au);
                   if (st) {   // 会话还被别人占着（通话没挂就切回游戏、打断结束时人还在别的 App）：不致命，继续由静音接收端顶着，等下一次恢复再试 | The session is still held by someone else (switched back to the game with a call not yet hung up, or the user is still in another app when the interruption ends): not fatal, the silent sink keeps covering, and we retry at the next resume
                       LOG("【红】AudioUnit 恢复失败（状态 %d）：保持静音接收端，等下次恢复重试", (int)st);
                       atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); failed = 1; }
#elif defined(TARGET_OS_OHOS) && TARGET_OS_OHOS
                   int ohrc = dm_audio_ohos.start(a->au);
                   if (ohrc) {   // 同款兜底：起不来就继续由静音接收端顶着，等下一次恢复再试 | Same fallback: if it does not come up, the silent sink keeps covering, and we retry at the next resume
                       LOG("【红】OHAudio 恢复失败（错误 %d）：保持静音接收端，等下次恢复重试", ohrc);
                       if (!a->ring) { atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); } failed = 1; }
#endif
            }
        }
        if (failed) g_suspended = 1;   // 没恢复成：状态留在「暂停」，下次调 0 会再试 | Resume did not succeed: the state stays "suspended", and the next call with 0 will retry
        LOG("audio %s", on ? "暂停（切后台 / 被打断）" : failed ? "恢复未成" : "恢复（回前台 / 打断结束）");
    }
    pthread_mutex_unlock(&g_aos_lock);
}
