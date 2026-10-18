// OHOS 重放层占位（M1）：NVN 执行走空实现，把整机先跑到「能进主菜单黑屏」；完整 Vulkan 重放（nvn_vulkan.c）是 M2。
// OHOS replay-layer placeholder (M1): NVN execution is a null implementation so the whole host can first reach "main menu, black screen"; the full Vulkan replay (nvn_vulkan.c) is M2.
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static _Atomic int g_red;
static void red(const char *fn) {
    if (!atomic_exchange_explicit(&g_red, 1, memory_order_relaxed))
        fprintf(stderr, "[dm] 【红】NVN 重放层（OHOS 空设备占位）：%s 首次被调用——图形无输出（M1 预期）\n", fn);
}

void bf_want_note(const char *key) { (void)key; }
void mtl_init(void) { fprintf(stderr, "[dm] NVN 重放层：OHOS 空设备占位已初始化（无图形输出）\n"); }
void mtl_pool_add(const void *mem, uint64_t size) { (void)mem; (void)size; }
void mtl_pool_del(const void *mem) { (void)mem; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { (void)addr; (void)len; if (buf) *buf = NULL; if (off) *off = 0; red("mtl_addr"); return 0; }
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) { (void)addr; (void)len; if (buf) *buf = NULL; if (off) *off = 0; if (gpu) *gpu = 0; return 0; }

// 提交路径：空设备下同步记账（事件/计数器立即完成，防游戏轮询卡死）
void mtl_sub_begin(void) {}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec(const uint8_t *handle) { (void)handle; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { (void)src; (void)dst; (void)n; }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) {
    if (!val) return;
    if (mode == 1) __atomic_fetch_add((uint32_t *)val, v, __ATOMIC_SEQ_CST);
    else __atomic_store_n((uint32_t *)val, v, __ATOMIC_SEQ_CST);
}
void mtl_sub_counter(uint64_t addr, uint32_t type) { (void)addr; (void)type; }
static _Atomic uint64_t g_fence;
uint64_t mtl_sub_commit(void) { return atomic_fetch_add_explicit(&g_fence, 1, memory_order_acq_rel) + 1; }
uint64_t mtl_last_fence(void) { return atomic_load_explicit(&g_fence, memory_order_acquire); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) { (void)on; (void)px; (void)py; (void)grid; (void)cx; (void)cy; }
int mtl_ui_ghost_rect(float r[4]) { (void)r; return 0; }

// 资源登记面（M2 的 nvn_vulkan.c 会真实现：贴图/采样器/程序的 Vulkan 侧生命周期）
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)r;
    if (dst) memset(dst, 0, row * img);
    red("mtl_read_texels");
}
int mtl_fps_target(void) { return 60; }
int dm_nvn_ruler_truth(void) { return 0; }

// 出图与统计面（空设备：出图不做事但票号递增，等待立刻返回；统计全 0）
static _Atomic uint64_t g_present_ticket;
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) { (void)t; (void)period_s; (void)crop; red("mtl_present"); return atomic_fetch_add_explicit(&g_present_ticket, 1, memory_order_acq_rel) + 1; }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns) { (void)timeout_ns; return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { if (passes) *passes = 0; if (draws) *draws = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { if (load) *load = 0; if (store) *store = 0; if (full_rw_passes) *full_rw_passes = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { if (call) *call = 0; if (skip) *skip = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { if (enc) *enc = 0; if (reuse) *reuse = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { if (lib) *lib = 0; if (pso) *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { if (uploads) *uploads = 0; if (reuploads) *reuploads = 0; }
void dm_gfx_shot_at(long off, const char *path) { (void)off; (void)path; red("dm_gfx_shot_at"); }

int mtl_display_hz(void) { return 0; }
int mtl_has_window(void) { return 0; }
double mtl_presented_time(uint64_t n) { (void)n; return 0.0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_alloc_stats(uint64_t *aw, uint64_t *ag, int *an) { if (aw) *aw = 0; if (ag) *ag = 0; if (an) *an = 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb, int heap_ok, unsigned long long heap_mb, unsigned long long heap_free_mb, unsigned long long heap_maxblk_kb) { (void)frame; (void)rss_mb; (void)heap_ok; (void)heap_mb; (void)heap_free_mb; (void)heap_maxblk_kb; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0.0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }
