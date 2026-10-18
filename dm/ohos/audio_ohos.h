// 平台出声后端接缝：鸿蒙（OHOS）上 hle_audio.c 经这张函数指针表驱动 OHAudio 渲染器；实现在 audio_ohos.c。
// Platform audio-output backend seam: on OHOS, hle_audio.c drives the OHAudio renderer through this function-pointer table; implementation in audio_ohos.c.
#pragma once
#include <stdint.h>

typedef struct {
    // 建渲染器（48 kHz / PCM16 / ch 声道交织）；fill 在音频回调线程被调，往里灌 bytes 字节；返回句柄，NULL = 失败（上层退静音接收端）
    // Create the renderer (48 kHz / PCM16 interleaved / ch channels); fill runs on the audio callback thread, handing it bytes; returns the handle, NULL = failure (the caller degrades to the silent sink)
    void *(*open)(int ch, void *ctx, void (*fill)(void *ctx, uint8_t *dst, uint64_t bytes));
    int (*start)(void *h);    // 0 = 成功 | 0 = success
    int (*stop)(void *h);
    void (*close)(void *h);
} dm_audio_backend;
extern const dm_audio_backend dm_audio_ohos;

// 诊断：出声回调请求帧数 / 实交付帧数 / 最后一次错误，经 dm/host 的 LOG 打印。
// Diagnostic: frames requested by the output callback vs. frames delivered, plus the last error, printed through LOG from dm/host.
void dm_audio_ohos_diag(void);
