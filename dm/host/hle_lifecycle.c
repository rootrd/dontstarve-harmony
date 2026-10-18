// 宿主生命周期：优雅退出（SIGTERM；iOS 由 App 终止回调调 dm_host_shutdown）。
// 停后台线程（启动块预取）、暂停音频、图形挂起（等在途 NVN 调用结束并清空 GPU 工作），再 _exit(0)；1.5 秒兜底直接 _exit(0)。
// 缺省动作同样约 0.08 秒退出（退出码 143）；本函数的价值是有序停后台线程、退出码 0、给 iOS 生命周期回调一个入口。
// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Stop background threads (boot block prefetch), pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void tact_prefetch_stop(void);
void granny_prof_dump(void);
void hlecount_flush(void);
#if !defined(__APPLE__)
static void *dm_shutdown_watchdog(void *p) {   // 1.5 秒兜底（对应 GCD dispatch_after）
    (void)p; struct timespec ts = {1, 500000000}; nanosleep(&ts, NULL);
    LOG("【红】优雅退出 1.5 秒未完成，直接退出"); _exit(0); return NULL;
}
#endif
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("收到 %s：优雅退出", why);
#if defined(__APPLE__)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("【红】优雅退出 1.5 秒未完成，直接退出"); _exit(0); });
#else
    { pthread_t td; if (!pthread_create(&td, NULL, dm_shutdown_watchdog, NULL)) pthread_detach(td); }
#endif
    tact_prefetch_stop();
    granny_prof_dump();
    hlecount_flush();                               // DM_HLECOUNT：_exit 不走 atexit，这里写一份 | DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("优雅退出完成（%.3f 秒）", (now_ns() - t0) / 1e9);
    _exit(0);
}
#if defined(__APPLE__)
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);                       // 不走缺省动作：交给 GCD 信号源在普通线程上收尾 | do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
}
#else
static void *term_sigwait(void *p) {   // OHOS：sigwait 线程在普通线程上收尾（对应 GCD 信号源） | OHOS: sigwait thread wraps up on an ordinary thread (counterpart of the GCD signal source)
    (void)p; sigset_t s; sigemptyset(&s); sigaddset(&s, SIGTERM);
    for (;;) { int sig = 0; if (sigwait(&s, &sig) == 0 && sig == SIGTERM) dm_host_shutdown("SIGTERM"); }
    return NULL;
}
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);
    pthread_t t; if (!pthread_create(&t, NULL, term_sigwait, NULL)) pthread_detach(t);
}
#endif
