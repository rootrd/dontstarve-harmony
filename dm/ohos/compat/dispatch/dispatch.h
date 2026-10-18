// OHOS 兼容替身：GCD 信号量（hle_audio.c 用到的面）→ pthread 条件变量实现
#pragma once
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#define DISPATCH_TIME_FOREVER (~0ull)
typedef struct { pthread_mutex_t m; pthread_cond_t c; int v; } oc_dispatch_sem_s;
typedef oc_dispatch_sem_s *dispatch_semaphore_t;
static inline dispatch_semaphore_t dispatch_semaphore_create(long v) {
    oc_dispatch_sem_s *s = (oc_dispatch_sem_s *)calloc(1, sizeof *s);
    if (!s) return 0;
    pthread_mutex_init(&s->m, 0); pthread_cond_init(&s->c, 0); s->v = (int)v;
    return s;
}
static inline int dispatch_semaphore_signal(dispatch_semaphore_t s) {
    pthread_mutex_lock(&s->m); s->v++; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); return 0;
}
static inline int dispatch_semaphore_wait(dispatch_semaphore_t s, unsigned long long timeout) {
    pthread_mutex_lock(&s->m);
    while (s->v <= 0) {
        if (timeout == DISPATCH_TIME_FOREVER) { pthread_cond_wait(&s->c, &s->m); }
        else { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
               ts.tv_nsec += (long)(timeout % 1000000000ull);
               ts.tv_sec += (time_t)(timeout / 1000000000ull) + ts.tv_nsec / 1000000000;
               ts.tv_nsec %= 1000000000;
               if (pthread_cond_timedwait(&s->c, &s->m, &ts)) break; }
    }
    if (s->v > 0) s->v--;
    pthread_mutex_unlock(&s->m);
    return 0;
}
static inline void oc_dispatch_release_impl(dispatch_semaphore_t s) { (void)s; }
static inline void dispatch_release(dispatch_semaphore_t s) { (void)s; }
