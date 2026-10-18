// NVN → Vulkan 重放层（OHOS / Maleoon）。对上层说同一门 C 语言：nvn_mtl.h + nvn_obj.h:124-176 的 mtl_* 面，
// 与 macOS/iOS 的 nvn_mtl.m 一一对应（那边是参照实现）。开关：DM_VK_GFX 0 = 整个层空转（回到 M1 的黑屏基线）。
// 本刀（M2-1）范围：设备/交换链/贴图即建即清/通道内清除/出图 blit。绘制、描述符、上传解码在后续刀次补，
// 一切没实现的入口都记数并静默返回，绝不让宿主崩（M1 的「跑到帧 13200+」不能被图形层拖回 1 秒退出）。
// NVN -> Vulkan replay layer (OHOS / Maleoon). To the layers above it speaks the same C: the mtl_* surface of nvn_mtl.h + nvn_obj.h:124-176,
// matching nvn_mtl.m on macOS/iOS 1:1 (that file is the reference implementation). Gate: DM_VK_GFX 0 = the whole layer idles (back to the M1 black-screen baseline).
// This cut (M2-1): device / swapchain / textures created on demand / clears inside a pass / present by blit. Draws, descriptors and pixel upload come in later cuts;
// every entry point not implemented yet only counts and returns silently - it must never crash the host (M1's "reached frame 13200+" must not be dragged back to a 1-second exit).
#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <native_window/external_window.h>
#include <hilog/log.h>
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include "nvn_tex.h"
#include "dmsh_chain.h"
#include "nvn_sh_tri.h"
#include "nvn_shbin.h"      // 构建机折出的着色器二进制库（shaders.bin）的布局 | layout of the binary shader library packed by the build box
#include "nvn_dirty.h"      // 客体内存写保护脏页：打包贴图上传后要能察觉游戏又写了它 | guest write-protect dirty tracking, so a packaged upload can notice the game rewrote it
#include <CommonCrypto/CommonDigest.h>   // OHOS 替身：oc_sha256（compat/sha256.c），与 nvn_mtl.m 的 CC_SHA256 同一门 | compat shim, same call site as nvn_mtl.m
#include "../ohos/xcomp_ohos.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "D2R"
#define LOG_DOMAIN 0x3300
#define GVL(...) do { OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__); dm_logfmt(__VA_ARGS__); } while (0)

// 日志双写：hilog 是环形缓冲，客体一刷屏就把一次性现场行挤掉——第 110 轮实测「空图 serial=」「打包上传」「探针」三类现场行
// 一条都没落盘，只剩周期计数器，等于闭眼改代码。DM_LOGFILE 指到应用 files 目录时同一行再写进文件，跑完 hdc file recv 拿全文。
// hilog is a ring buffer and the guest's log flood rotates one-shot scene lines out of it: round 110 lost every "empty image",
// "packaged upload" and probe line, leaving only the periodic counters. With DM_LOGFILE set, each line also goes to that file,
// so a round can be read back complete with hdc file recv.
static FILE *g_lf;
static pthread_mutex_t g_lfm = PTHREAD_MUTEX_INITIALIZER;
static void dm_logfmt(const char *fmt, ...)
{
    if (!g_lf) {
        const char *p = getenv("DM_LOGFILE");
        if (!p || !*p) return;
        char path[1024];
        if (p[0] == '/') snprintf(path, sizeof path, "%s", p);
        else {   // DM_LOGFILE=1（或裸文件名）：落到客体自己的沙箱临时目录，hdc 能从 /data/app/el2/100/base/<包>/... 读回来
            const char *d = getenv("DM_TEMP");
            if (!d || !*d) d = getenv("DM_SAVE");
            if (!d || !*d) return;
            snprintf(path, sizeof path, "%s/%s", d, strcmp(p, "1") == 0 ? "vk.log" : p);
        }
        pthread_mutex_lock(&g_lfm);
        if (!g_lf) g_lf = fopen(path, "ae");      // 惰性开：起层之前打的行也不该因为没开文件而丢
        pthread_mutex_unlock(&g_lfm);
        if (!g_lf) return;
    }
    char tf[1024]; int j = 0;
    for (const char *p = fmt; *p && j < (int)sizeof tf - 4; ) {
        if (p[0] == '%' && p[1] == '{') {      // hilog 的可见性标记不是 printf 转换，musl 认不得：换成普通 %
            const char *e = strchr(p, '}');
            if (e) { tf[j++] = '%'; p = e + 1; continue; }
        }
        tf[j++] = *p++;
    }
    tf[j] = 0;
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, tf, ap);
    va_end(ap);
    if (n <= 0) return;
    pthread_mutex_lock(&g_lfm);
    if (g_lf) { fwrite(buf, 1, (size_t)n, g_lf); fputc('\n', g_lf); fflush(g_lf); }
    pthread_mutex_unlock(&g_lfm);
}

#define GLOG_MAX 200          // 每种日志最多打几条，防止刷爆 hilog 环形缓冲 | how many lines of each kind before they are suppressed (keeps the hilog ring buffer alive)

static int g_on;              // DM_VK_GFX 且设备起来了 | DM_VK_GFX set and the device is up
static int g_skip;            // DM_VK_SKIP 位图（定位驱动侧故障用，逐类关掉再对比）| bisect knob, turn one class off at a time:
                              // 1 = 不开通道 2 = 不发清除 4 = 通道不挂深度 8 = 不出图 16 = 出图不搬像素 32 = 提交但不呈现
                              // 64 = 通道照录但呈现不用可见信号量（改等出图栅栏）128 = 出图提交前先排空队列 256 = 游戏提交整个不上 GPU
                              // 512 = 出图通道写离屏图（交换链那张不碰，present 照走）1024 = 出图只清色、不画满屏三角形
                              // 2048 = 不重放游戏自己的绘制（M2-3b 的回退位：通道/清除/出图照旧）
                              // 67108864 = 打包贴图只传一次：不设只读、不查脏、不重传（B49 的回退位，用来 A/B「游戏又写了这张」这一刀）
                              // 65536（第 112 轮改义）= 关掉「按池偏移认图 + 释放进回收站」，退回按 serial 建图、Finalize 当场销毁的旧行为
                              // 33554432（第 112 轮改义）= 哑图退回彩色渐变；不设 = 不透明纯黑（现在的默认）
                              // 268435456 = 混合启用的附件把源因子一律顶成 ONE（B45 探针，从 33554432 挪来）
                              // 536870912 = 关掉「同一块池内存的新生命按重写过处理、补一次打包重传」
                              // 1 = no render passes, 2 = no clears, 4 = passes without the depth attachment, 8 = no present,
                              // 16 = present moves no pixels, 32 = present submits but skips vkQueuePresentKHR,
                              // 64 = pass recorded but present waits the fence instead of the visible semaphore, 128 = drain the queue before the present submit,
                              // 256 = the game's submission never reaches the GPU at all, 512 = the present pass writes an offscreen image instead of the swapchain,
                              // 1024 = the present pass only clears - no full-screen triangle draw
static int g_red_once;
static int g_sub_tid, g_pres_tid;   // 现场：游戏提交与出图各自的 OS 线程号（-1 = 该入口在多个线程上出现过）
                                    // Tids of the game submission path and the present path (-1 = seen on more than one thread)
#define GRED_MAX 200          // 红线上限：一次真机排查要打现场，12 条不够用 | fault lines: 12 was too few to see the whole scene
// 标记用 ASCII 的 [red]：hilog 落盘后中文串在本机 grep 不可靠，纯 ASCII 才捞得到
// The marker is ASCII "[red]": grepping CJK substrings out of a dumped hilog is unreliable, ASCII markers actually get found.
#define GRED(...) do { if (g_red_once < GRED_MAX) { g_red_once++; GVL("[red] " __VA_ARGS__); } } while (0)
static void tri_invalidate(void);   // 定义在下面：交换链重建会让 g_sc_rp 换新，指着旧通道的管线必须先销毁 | defined below: rebuilding the swapchain replaces g_sc_rp, and the pipeline built against it has to go first
static int host_writable(const void *p);   // 定义在下面：mincore 逐页判可读（探针要读游戏给的控制段）| defined below: per-page mincore readability, the probe reads control segments handed over by the game
static void vk_probe_dump(void);   // 定义在下面：op 直方图 + 着色器键汇总 | defined below: op histogram + shader key summary
static int vk_ensure(void);   // 见文件末尾：惰性起层 | see the bottom of the file: lazy boot
static void vk_read_limits(void);        // 定义在 M2-3b 段：起层后读实测上限 | defined in the M2-3b block: read the measured limits after boot
static void mtl_sub_reset_draw(void);    // 定义在 M2-3b 段：换提交清 arena / 描述符 / 顶点流 | defined in the M2-3b block: a new submission starts empty
static void draw_stats_line(char *line, int m, int cap);   // 定义在 M2-3b 段：绘制重放计数 | defined in the M2-3b block: replay counters
static void pipe_fail_dump(void);   // 定义在 M2-3b 段：管线建失败的现场清单（每对一行 ASCII）| defined in the M2-3b block: the pipeline failure scene list, one ASCII line per pair
static int shb_pair(const char *k0, const char *k1, int *vi, int *fi);   // 定义在 M2-3b 段：顶/像段键对 → 库条目号 | defined in the M2-3b block: vertex+fragment keys -> entry indices
static int shb_stage(const char *k, int *vi, int *fi);   // 一个键自己查库：它是顶段还是像段（饥荒一段一个程序，绑定两次才凑齐一对）| look one key up in the library: vertex or fragment (Don't Starve binds one stage per program, so a pair takes two binds)

// ---------------- Vulkan 现场 | Vulkan state ----------------
static VkInstance g_inst;
static VkSurfaceKHR g_surf;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_q;
static uint32_t g_qf;
static VkCommandPool g_cp;
static uint32_t g_mem_host_visible;      // memType 位图里第一个 host-visible|coherent 的号 | the first memory type index with HOST_VISIBLE|HOST_COHERENT
static uint32_t g_mem_dev_local;

static VkSwapchainKHR g_sc;
static uint32_t g_sc_n;
static VkImage g_sc_img[8];
static VkImageView g_sc_view[8];
static VkFormat g_sc_fmt;
static VkExtent2D g_sc_ext;
static VkSemaphore g_acq_sem[8];
static VkRenderPass g_sc_rp;               // 出图用经典渲染通道 + 每帧缓冲一个：M2-0 冒烟实测唯一绿的写法（动态渲染对交换链图会打出 GPU 故障）
                                           // Present uses a classic render pass plus one framebuffer per image: the recipe M2-0 proved green
                                           // (dynamic rendering onto a swapchain image takes the GPU down)
static VkFramebuffer g_sc_fb[8];
static VkImage g_off_img;                 // 位 512 用：与交换链同尺寸的同格式离屏图，通道写它而不写交换链
static VkDeviceMemory g_off_mem;
static VkImageView g_off_view;
static VkFramebuffer g_off_fb;
static VkSemaphore g_vis_sem;              // 出图提交 → vkQueuePresentKHR 的可见信号量 | signals the present submit, present waits it
static int g_sc_ready;

static VkCommandBuffer g_cb;             // 本次提交的命令缓冲 | this submission's command buffer
static VkFence g_fence;
static VkFence g_pfence;                 // 出图提交专用：与游戏的提交栅栏分开，避免一个栅栏两次在用 | present submissions get their own fence, so one fence is never submitted twice
static uint64_t g_fence_val;
static uint64_t g_submits, g_presents;
#define DUMP_EVERY 60      // 出图节拍：每 60 帧做一次读回 + 打一遍绘制侧记账。300 帧在这台机要 75 秒（一帧 250 ms），一整轮实测都等不到第二次 dump
static int g_in_pass;
// 通道没开成的原因计数（M2-3b 定位 无通道 用）| why a game render pass did not open (used to locate the 无通道 half of M2-3b)
static _Atomic uint64_t g_rt_noatt, g_rt_nocb, g_rt_rp, g_rt_fb;
static uint64_t g_buf_ops[OP_COUNT_];   // 本次提交里每种 op 的条数：驱动报错时把这条缓冲的构成打出来
                                        // Per-submission op histogram: when the driver faults, print what this buffer was made of

// 贴图表：serial → VkImage（游戏按值拷贝 NVN 对象，只能靠 serial 认）| texture table: serial -> VkImage (the game copies NVN objects by value, so only the serial identifies them)
typedef struct {
    uint64_t serial;
    tex_t d;
    VkImage img;
    VkImageView view;
    VkDeviceMemory mem;
    VkFormat fmt;
    VkImageLayout layout;
    uint32_t w, h, layers, levels;
    int is_depth;
    int soft_bc;                         // NVN 是 BC 块、设备一个 BC 都不认（第 81 轮实测）：像素要在 CPU 解码后上传，Vk 图按格式是 R8/RG8/RGBA8/RGBA16F
    int uploaded;                        // 已经把池里的内容送进显卡（后续刀次用）| pool content has been pushed to the device (later cuts)
    int pkg_bad;                         // 打包上传试过但失败（格式不认 / 越界 / arena 满）：不再反复排队 | packaged upload attempted and refused - stop re-queuing it
    int pkg_tried;                       // 量过一张（用来把「排队次数」和「张数」分开）| counted once per texture so the queue counter means textures
    uint64_t pkg_stamp, pkg_seen;        // 上传时客体内存池的写代号 / 上次查脏时代号（代号没动就不逐页查）| generation at upload, and the generation last inspected (no page scan while it stands still)
    uint64_t pkg_range;                  // 这张图在池里占的字节数（级 0 起点算起，脏检查与设只读的范围）| bytes this texture occupies in the pool - the range the dirty check and the write-protect cover
    int pkg_dirty;                       // 池里这张图的页在上传后被动过：等通道外重传 | the pool pages moved after the upload - re-upload outside the pass
    int pkg_stale;                       // 这张在池里的位置上一世传过，游戏又在同一块字节上重新 Initialize：显存里那份是旧数据，用之前得重传
                                         // This pool spot was uploaded in a previous life and the game re-initialized over it: the device copy is stale
    int pkg_rej;                         // 暂存圈不到几次了：连拒 8 次就不再排队（不置 pkg_bad 是为了让「下次提交游标清零」有机会救它）| staging refusals - after 8 give up, while still allowing a later submission to win
    uint64_t ph_st, ph_last;             // 比池内容是在第几拍出图 / 那次的哈希：缺页钩子在 OHOS 上不响，改按内容比对判脏
                                         // Which present we last compared this texture's pool bytes, and that hash: the write-protect fault stays silent on OHOS, so dirtiness comes from comparing content
    uint64_t vk_bytes;                   // vkGetImageMemoryRequirements 给的这块显存大小（回收站按它算预算）
    uint64_t recy_seq;                   // 进回收站的先后（只在回收态有意义，淘汰取最小的）
    int drawn;                           // 当过至少一趟的附件（渲染目标也算有来源）| has been a pass attachment at least once
    int sibchk, sib;                     // 同 serial 的兄弟实例当时有没有内容：画进 A、采的是 B 的直接判据（一次算死，别每次扫表）
                                         // Whether a sibling VkImage sharing this serial had content: the draw-into-A / sample-B signature, resolved once
} vktex;
#define VTN 16384            // 4096 装不下：按需建图的光要 1 万多个槽（游戏登记 10491 张贴图），一张格 ~120 字节，抬到 16384 才 2 MB
static vktex g_tex[VTN];
static int g_tex_n;
static uint64_t g_tex_new, g_tex_hit, g_tex_noformat;
static uint64_t g_tex_dupser;             // 同一 serial 建出第二张图（画与采分家的判据）| a second image under one serial = draw and sample split apart

// ---- 贴图身份：池里的位置，不是 NVN 的 serial ----
// Texture identity: where the bytes sit in the pool, not the NVN serial
// 第 112 轮实测：哑图绑定 3965 次，其中 3952 次落在「本帧刚新建的那张」上（按需新建同样是 3952，同 serial 二次建图只有 1 次）。
// 空图清单里 serial 每帧一路 942→968 往上跳，尺寸固定在 1280x720 / 640x360 那几张，偏移在 11821568、12805120、210374144…
// 之间来回循环 = 饥荒前端每帧把同一块池内存上的贴图 Finalize 掉再 Initialize。按 serial 认图 + Finalize 当场销毁 VkImage
// ⇒ 每帧都得建一张全新的空图，本帧采样只能落回哑图，字、菜单行、辉光链永远是缺的（上传要等到通道外才发得出）。
// Round 112 measured 3965 dummy binds, 3952 of them on an image created that very frame (new-image count was also 3952, and only one
// duplicate serial existed). The empty list walks serials 942->968 every frame over the same 1280x720 / 640x360 sizes, with the pool
// offsets cycling through 11821568 / 12805120 / 210374144 ...: DST's front end finalizes and re-initializes the very same pool storage
// each frame. Keying on the serial and destroying the VkImage at finalize therefore builds a brand new empty image every frame, which
// can only sample the dummy that frame - text, menu rows and the bloom chain stay missing forever, because the upload cannot be issued
// until outside the pass.
// 两条动作：①描述符 + 池 + 偏移相同就是同一张图，活表里直接复用（顺带钉死「画进 A、采的是 B」）；
// ②Finalize 不再销毁，把 VkImage 挪进回收站，同身份重建时带着上一世的像素复活。位 65536 关掉这两条，退回旧行为做 A/B。
// Two moves: (1) same descriptor + pool + offset is the same image - reuse it from the live table, which also closes the
// draw-into-A / sample-B split; (2) finalize parks the VkImage in a recycle bin instead of destroying it, so a rebuild with the same
// identity comes back with last life's pixels. Bit 65536 turns both off and restores the old behaviour for the A/B.
#define RECY_N 128                              // 回收站格数：稳态就那几十张 UI/辉光图在轮回 | slots - the steady state is a few dozen images cycling
#define RECY_BYTES (512ull << 20)               // 回收站显存预算，超了就淘汰最久的那条（淘汰 = 真销毁）| device-memory budget; over it, the oldest entry is really destroyed
static vktex g_recy[RECY_N];
static int g_recy_n;
static uint64_t g_recy_bytes, g_recy_seq, g_recy_put, g_recy_lost, g_tex_stor, g_tex_revive, g_tex_stormis, g_tex_stale;

// ---- 按内容比对判脏 | dirtiness by comparing the pool bytes ----
// 打包贴图（开局那段视频、LOADING 横幅）的语义是「描述符指向池内存，GPU 每次读当前内容」，所以游戏一写就得重传。
// 原来这条靠把覆盖页设成只读、等缺页：本机一轮只响两次缺页，等于没有（第 24 号遗留项）。
// 改成每次采样前自己比一下池里的字节——取五个点（头、1/4、1/2、3/4、尾前）各 8 字节算个哈希，和上一拍比。
// 视频那种整幅重写的图，五个点全都会变；静态图五点不动，一次也不重传。每拍最多重传 8 张，超了就记账。
// Packaged textures (the boot video, the LOADING banner) mean "the descriptor points at pool memory and the GPU reads the current
// bytes", so a guest write has to trigger a re-upload. That used to wait for a write-protect fault, and this device faults twice a
// round - which is no signal at all. So compare the content instead: hash five sampled points of the pool span once per present and
// re-upload when it moved. A full-frame video rewrite moves all five; a static image moves none and never re-uploads.
static _Atomic uint64_t g_pw_chg, g_pw_same, g_pw_que, g_pw_cap;
static int g_poolwatch = 1;

static uint64_t pool_hash(const uint8_t *p, uint64_t n)
{
    if (!p || n < 32) return 0;
    uint64_t h = 1469598103934663733ULL;
    for (int i = 0; i < 5; i++) {
        uint64_t v;
        memcpy(&v, p + (n - 32) * (uint64_t)i / 4u, 8);
        h = (h ^ v) * 1099511628211ULL;
    }
    return h;
}

static int tex_same_store(const vktex *v, const tex_t *t)
{
    return t->pool && t->mem && v->d.pool == t->pool && v->d.off == t->off &&
           memcmp(&v->d.d, &t->d, sizeof t->d) == 0 && v->d.L.layers == t->L.layers;
}

// 只销毁 Vulkan 对象，槽位由调用方摘（memset 与 g_tex_n 收缩都得由知道自己在表哪一格的人做）
// Destroys only the Vulkan objects; the caller removes the slot, since only it knows where the entry sits
static void tex_destroy(vktex *v)
{
    if (v->view) vkDestroyImageView(g_dev, v->view, NULL);
    if (v->img) vkDestroyImage(g_dev, v->img, NULL);
    if (v->mem) vkFreeMemory(g_dev, v->mem, NULL);
    v->view = (VkImageView)0; v->img = (VkImage)0; v->mem = (VkDeviceMemory)0;
}

// 回收站进：这一世的状态整份搬进去（含已上传的像素、脏页戳与只读保护范围），下一世同身份拿来就能用
// Parking a whole state in the bin - uploaded pixels, dirty stamp and protected range included - so the next life starts usable
static int recy_put(const vktex *v)
{
    if (!v->img) return 0;
    while (g_recy_n >= RECY_N || g_recy_bytes + v->vk_bytes > RECY_BYTES) {
        int k = -1;
        uint64_t best = ~(uint64_t)0;
        for (int i = 0; i < g_recy_n; i++) if (g_recy[i].recy_seq < best) { best = g_recy[i].recy_seq; k = i; }
        if (k < 0) return 0;                       // 这张自己就超预算：不收，照旧销毁
        tex_destroy(&g_recy[k]);
        g_recy_bytes -= g_recy[k].vk_bytes;
        g_recy_lost++;
        g_recy[k] = g_recy[--g_recy_n];
        memset(&g_recy[g_recy_n], 0, sizeof g_recy[g_recy_n]);
    }
    g_recy[g_recy_n] = *v;
    g_recy[g_recy_n].recy_seq = ++g_recy_seq;
    g_recy_bytes += v->vk_bytes;
    g_recy_n++;
    g_recy_put++;
    return 1;
}

// 序号 → 表槽的哈希：描述符每填一组绑定都要按序号找视图（一次通道几十条），线性扫 3 千张会把重放拖垮。
// 增删贴图只进位 g_tex_gen，读端发现代号不符就整表重建（重建 3 千项，比每笔绘制扫 3 千项便宜得多）。
// serial -> slot hash: filling one descriptor set looks up dozens of textures by serial, and a linear scan over 3k entries per draw
// would stall the replay. Creates / releases only bump g_tex_gen; a reader whose copy is stale rebuilds the whole table once.
static uint64_t g_tex_gen, g_tex_idx_gen;
static int g_tex_idx[2048];
static unsigned tex_hash(uint64_t s)
{
    s ^= s >> 33; s *= 0xff51afd7ed558ccdull; s ^= s >> 29;
    return (unsigned)(s & 2047u);
}
static void tex_idx_build(void)
{
    memset(g_tex_idx, 0, sizeof g_tex_idx);
    for (int i = 0; i < g_tex_n; i++) {
        unsigned q = tex_hash(g_tex[i].serial);
        for (int t = 0; t < (int)(sizeof g_tex_idx / sizeof g_tex_idx[0]); t++) {
            unsigned p = (q + (unsigned)t) & 2047u;
            if (!g_tex_idx[p]) { g_tex_idx[p] = i + 1; break; }
        }
    }
    g_tex_idx_gen = g_tex_gen;
}

static void to_layout(VkCommandBuffer cb, vktex *v, VkImageLayout want);   // 定义在下面：出图管线要先用它把源图转成可采样布局 | used by the present pipeline, defined below
static VkImageLayout col_rest(void);   // 定义在 rp_get 之前：通道外颜色附件停的那一态（位 4096 可退回旧写法）
static VkImageLayout dep_rest(void);   // 同上，深度侧（位 8192）
static int g_sets_valid;      // 定义在下面：贴图释放时要作废缓存的描述符集 | defined below; releasing a texture invalidates the cached sets

// 「游戏有没有往出图源那张画」这一问的记账 | bookkeeping for "does the game ever draw into the present source?"
static vktex *g_src_v[4];                // 出图用过的几张（交换链轮转，最多 4）
static _Atomic uint64_t g_src_pres[4];   // 每张被「上屏」过多少次：挂通道次数只说明画过，这一列才说明游戏拿哪张去呈现
static int g_src_n;
// 每张出图源被挂进游戏通道的次数：游戏是三缓冲，只统计「一共挂过几次」分不出画进了哪一张，
// 而读回只读到其中一张（第 92 轮实测 s=5 读回全零，屏幕上却有内容）。
// How many game passes each present source was attached to. The game triple-buffers, and an aggregate count cannot say which
// image the draws landed in - while the readback only ever sampled one of them (round 92: source 5 read back all zero, yet the
// screen had content).
static _Atomic uint64_t g_src_att[4];
static _Atomic uint64_t g_rt_src_pass;   // 把它当颜色附件挂上的通道条数：0 = 游戏从没画它
static struct { uint32_t w, h; _Atomic uint64_t n; } g_rtsize[8];   // 游戏通道尺寸分布（前 8 种）
static struct { int f; _Atomic uint64_t n; } g_txfmt[24];           // 新建贴图按 NVN 格式计数（搬像素那一刀要先知道有没有 BC；第 82 轮 10 格装不下，BC7 有没有用看不出来）
// 槽号对照（第 86 轮定位「绑图 94% 无号」）：游戏每次 BindSeparateTextures 往哪个 (阶段,槽) 写非零句柄，
// 对比着色器反射要用的 (阶段,槽) 哪些是空的。两边对不上就是贴图永远采不到。
// Slot cross-check (round 86, chasing 「94% of bindings have no handle」): which (stage, slot) pairs the game actually writes a
// non-zero handle into, versus which slots the shader reflection asks for and finds empty. A mismatch there is why nothing samples.
static _Atomic uint64_t g_slot_fill[6][32], g_samp_fill[6][32], g_bind_miss[32], g_bind_real_st[6];
// 采样器侧计数（登记 / 描述符用上真采样器 / 句柄查无登记 / vkCreateSampler 失败），定义在「哑资源」一节的注册表旁
static _Atomic uint64_t g_samp_reg, g_samp_use, g_samp_none, g_samp_bad;
// 哑的三种来源分开记 + 影子采样绑上真深度视图的次数（判据在绑定那节，声明放这里给统计块先用）
// The three flavours of dummy counted apart, plus how many shadow reads got a real depth view.
static _Atomic uint64_t g_b_noimg, g_b_depth_plain, g_b_sh_color, g_b_shadow;
// 真绑上一张从没传过像素的图（v->uploaded 为 0）：第 90 轮出图读回 RGB 全 0、alpha 却有 34% 落笔，
// 说明着色器确实画了，只是采到的纹理是空的——这一条数它到底占多少真绑。
// A real bind onto an image that never had pixels uploaded (uploaded == 0). Round 90's readback of the present source had
// RGB exactly 0 while 34% of the alpha was written, so the shaders drew but sampled empty textures; this counts how much of
// the real binding is that.
static _Atomic uint64_t g_b_neverup;
// 图建好了但还没有任何来源（既没传过也没画过）⇒ 只能继续用哑图
// Image exists but has no source yet (never uploaded, never an attachment) - the dummy stays.
static _Atomic uint64_t g_b_empty;
// 上传时探到的像素：有内容的张数 / 一个非零字节都没有的张数（第 91 轮实测软解出来全零，这一条分得开「取源错」和「解码错」）
// Pixel probe at upload time: textures that carried non-zero bytes versus those that were all zero. Round 91 measured the
// soft-decoded output as zero, so this splits "wrong source address" from "broken decoder".
static _Atomic uint64_t g_up_zero, g_up_content;
// 位 16384 把颜色真图退回哑图时，那些哑算哪一类：单独一格，免得混进「影子给颜色」。
// Which bucket the forced-dummy (bit 16384) colour binds fall into - kept separate so it does not pollute 「影子给颜色」.
static _Atomic uint64_t g_b_forced;
// 贴图槽号三个候选各命中多少（第 89 轮的判据）：分开句柄的绑定里 nvn_reg 存的是采样器 s，贴图 t 旧库没带出来。
// 候选 = bank（新库才有）/ SPIR-V binding 号 / reg。若 binding 号命中占大头，说明当年翻译器就是按 t 编的号，
// 这一刀不用等构建机重打包。
// Three candidates for the texture slot of a separate-handle binding: bank (only a repacked library has it), the SPIR-V binding
// number, and nvn_reg (which is really the sampler s). If the binding number resolves most of the time, the translator numbered
// bindings by t and this cut does not need the build box.
static _Atomic uint64_t g_cand_bank, g_cand_bind, g_cand_reg, g_cand_none;
// 「上屏那条通道里到底画没画」| does the on-screen pass actually get draws
static int g_att_src;                                                // 当前通道挂了出图源之一
static vktex *g_sp_tex[2];                                           // 上屏那条通道采的两张真图（读回窗口轮流读它们）
// 第 98 轮实测：上屏通道采的「输入1」自己就是黑的（RGB 全 0、alpha 全 255）⇒ 锅在它上游那条通道。
// 这一组数就是查那条通道：它开了几趟、里面重放了几绘。
// Round 98: the first input of the on-screen pass is itself black (RGB 0, alpha 255), so the fault is upstream.
// These counters answer how many passes attach that image and how many draws actually replay into it.
static _Atomic uint64_t g_in1_pass, g_in1_draw;
static int g_in1_cur;
static _Atomic uint64_t g_sp_draw, g_sp_replay;
static _Atomic uint64_t g_spb_real, g_spb_dummy;   // 上屏那条通道里的贴图绑定：真图 / 哑图各多少次 | texture binds inside a present-source pass, split real vs dummy
static _Atomic uint64_t g_dy_sib, g_dy_nosib;      // 采到空图那一下：同 serial 的兄弟已有内容（= 一张图被拆成两页）/ 谁都没内容（= 字节没进过 GPU）
                                                    // On a sampled-empty bind: a sibling under the same serial already had content (one image split in two) or nothing ever did (bytes never reached the GPU)
static int g_sp_why;                                                 // 第一次没画成的原因号，只报一次
static void sp_why(int code, int vi, int fi, int extra);
static void sp_why(int code, int vi, int fi, int extra)
{
    if (g_sp_why) return;
    if (!g_att_src) return;
    g_sp_why = code;
    GVL("[vk] 上屏通道首败 因%{public}d 顶%{public}d 像%{public}d 备%{public}d", code, vi, fi, extra);
}

// 计数（mtl_stats / mtl_res_counts 用）| counters (for mtl_stats / mtl_res_counts)
static _Atomic uint64_t g_passes, g_draws, g_clears, g_dispatches, g_copies;
static _Atomic uint64_t g_ops[OP_COUNT_];

static const char *vr(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "OK";
    case VK_TIMEOUT: return "TIMEOUT";
    case VK_NOT_READY: return "NOT_READY";
    case VK_INCOMPLETE: return "INCOMPLETE";
    case VK_ERROR_OUT_OF_DATE_KHR: return "OUT_OF_DATE";
    case VK_SUBOPTIMAL_KHR: return "SUBOPTIMAL";
    case VK_ERROR_SURFACE_LOST_KHR: return "SURFACE_LOST";
    default: return "ERR";
    }
}
#define GK(label, call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { GRED("%{public}s = %{public}d (%{public}s)", label, (int)_r, vr(_r)); } } while (0)

// NVNformat → VkFormat（本刀只覆盖窗口/通道会用到的那些；认不出的记数并回 0）
// NVNformat -> VkFormat (this cut covers what the window / render passes use; unknowns are counted and return 0)
static uint32_t g_bcap;   // 压缩格式能力位：1 BC1_RGB 2 BC1_RGBA 4 BC2 8 BC3 16 BC4 32 BC5 64 BC6H 128 BC7 256 ASTC | compressed-format support bits, probed once at boot

// NVN 格式里有几个颜色分量（0 = 不知道：压缩、深度、以及 _UI2F 那一族的别名，这些一律留给恒等映射）。
// 只算未压缩族，免得把软解码 BC 的错误也一起遮掉。
// Component count of an uncompressed NVN format; 0 = unknown (compressed, depth, the _UI2F aliases) and left to identity.
// Deliberately excludes the compressed families so a soft-decode bug cannot hide behind a forced alpha.
static int nvc_ch(int f)
{
    if (f >= NVN_FORMAT_R8 && f <= NVN_FORMAT_RGBA32I) return 1 + (f - NVN_FORMAT_R8) / 12;
    switch (f) {
    case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_RGB5: case NVN_FORMAT_RGB565:
    case NVN_FORMAT_R11G11B10F: case NVN_FORMAT_RGB9E5F:
    case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI:
    case NVN_FORMAT_BGR565: case NVN_FORMAT_BGR5: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
        return 3;
    case NVN_FORMAT_RGBA8_SRGB: case NVN_FORMAT_RGBA4: case NVN_FORMAT_RGB5A1:
    case NVN_FORMAT_RGB10A2: case NVN_FORMAT_RGB10A2UI:
    case NVN_FORMAT_BGRA8: case NVN_FORMAT_BGRA8_SRGB:
        return 4;
    default: return 0;
    }
}

static VkFormat fmt_of(int f, int *depth)
{
    if (depth) *depth = 0;
    switch (f) {
    // R / RG 族（第 37 轮实测游戏在用 0x5 0x6 0xB 0xD 0x3F，先补齐）
    // R / RG families (round 37 measured the game using 0x5 0x6 0xB 0xD 0x3F, so they go in now)
    case NVN_FORMAT_R8: return VK_FORMAT_R8_UNORM;
    case NVN_FORMAT_R8SN: return VK_FORMAT_R8_SNORM;
    case NVN_FORMAT_R8UI: return VK_FORMAT_R8_UINT;
    case NVN_FORMAT_R8I: return VK_FORMAT_R8_SINT;
    case NVN_FORMAT_R16F: return VK_FORMAT_R16_SFLOAT;
    case NVN_FORMAT_R16: return VK_FORMAT_R16_UNORM;
    case NVN_FORMAT_R16SN: return VK_FORMAT_R16_SNORM;
    case NVN_FORMAT_R16UI: return VK_FORMAT_R16_UINT;
    case NVN_FORMAT_R16I: return VK_FORMAT_R16_SINT;
    case NVN_FORMAT_R32F: return VK_FORMAT_R32_SFLOAT;
    case NVN_FORMAT_R32UI: return VK_FORMAT_R32_UINT;
    case NVN_FORMAT_R32I: return VK_FORMAT_R32_SINT;
    case NVN_FORMAT_RG8: return VK_FORMAT_R8G8_UNORM;
    case NVN_FORMAT_RG8SN: return VK_FORMAT_R8G8_SNORM;
    case NVN_FORMAT_RG8UI: return VK_FORMAT_R8G8_UINT;
    case NVN_FORMAT_RG8I: return VK_FORMAT_R8G8_SINT;
    case NVN_FORMAT_RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case NVN_FORMAT_RG16: return VK_FORMAT_R16G16_UNORM;
    case NVN_FORMAT_RG16SN: return VK_FORMAT_R16G16_SNORM;
    case NVN_FORMAT_RG16UI: return VK_FORMAT_R16G16_UINT;
    case NVN_FORMAT_RG16I: return VK_FORMAT_R16G16_SINT;
    case NVN_FORMAT_RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case NVN_FORMAT_RG32UI: return VK_FORMAT_R32G32_UINT;
    case NVN_FORMAT_RG32I: return VK_FORMAT_R32G32_SINT;
    case NVN_FORMAT_RGB8: return VK_FORMAT_R8G8B8_UNORM;
    case NVN_FORMAT_R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case NVN_FORMAT_RGB9E5F: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;        // RGBA8
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;    // RGBA16F
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x37: return VK_FORMAT_R8G8B8A8_SRGB;          // RGBX8 sRGB
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;    // RGB565
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;         // RGBX8
    case 0x95: return VK_FORMAT_B5G6R5_UNORM_PACK16;    // BGR565
    case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;         // BGRX8
    case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;         // BGRA8
    case 0x9B: return VK_FORMAT_B8G8R8A8_SRGB;
    case 0x9C: return VK_FORMAT_B8G8R8A8_SRGB;
    // 压缩格式：DXT / RGTC / BPTC（4×4 块）与 ASTC。马良收不收要问设备（g_bcap 由 vk_boot 探出来）。
    // 实测第 81 轮：Maleoon 920 只报 ASTC 4x4 可采样，BC1–BC7 全不认。而 D2R 的美术资源就是 BC1/BC3，
    // 所以认不下的 BC 块改建成未压缩（BC1–BC5/BC7 → RGBA8 或 R8/RG8，BC6H → RGBA16F），
    // 像素在 do_copy_b2t 里用 nvn_bcdec 的 CPU 解码搬进去（第 82 轮实测：上传 94 → 12244，「贴图没有图」12189 → 54）。
    // Measured on round 81: Maleoon 920 samples only ASTC 4x4 and rejects every BC format, while D2R's art is BC1/BC3 -
    // so the unsupported blocks are rebuilt uncompressed (BC1-BC5/BC7 -> RGBA8 or R8/RG8, BC6H -> RGBA16F) and the texels
    // are decoded on the CPU by nvn_bcdec inside do_copy_b2t (round 82: uploads 94 -> 12244, "no image" 12189 -> 54).
    // BC4 SNORM / BC5 SNORM 也按无符号解：符号错了只会让法线贴图偏色，不致命，等真要用了再补。
    case 0x41: return g_bcap & 1   ? VK_FORMAT_BC1_RGB_UNORM_BLOCK  : VK_FORMAT_R8G8B8A8_UNORM;
    case 0x45: return g_bcap & 1   ? VK_FORMAT_BC1_RGB_SRGB_BLOCK   : VK_FORMAT_R8G8B8A8_SRGB;
    case 0x42: return g_bcap & 2   ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
    case 0x46: return g_bcap & 2   ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK  : VK_FORMAT_R8G8B8A8_SRGB;
    case 0x43: return g_bcap & 4   ? VK_FORMAT_BC2_UNORM_BLOCK      : VK_FORMAT_R8G8B8A8_UNORM;
    case 0x47: return g_bcap & 4   ? VK_FORMAT_BC2_SRGB_BLOCK       : VK_FORMAT_R8G8B8A8_SRGB;
    case 0x44: return g_bcap & 8   ? VK_FORMAT_BC3_UNORM_BLOCK      : VK_FORMAT_R8G8B8A8_UNORM;
    case 0x48: return g_bcap & 8   ? VK_FORMAT_BC3_SRGB_BLOCK       : VK_FORMAT_R8G8B8A8_SRGB;
    case 0x49: return g_bcap & 16  ? VK_FORMAT_BC4_UNORM_BLOCK      : VK_FORMAT_R8_UNORM;      // 单通道：解出来就是 R8（nvn_bcdec 的 1 bpp 目标）
    case 0x4A: return g_bcap & 16  ? VK_FORMAT_BC4_SNORM_BLOCK      : VK_FORMAT_R8_SNORM;
    case 0x4B: return g_bcap & 32  ? VK_FORMAT_BC5_UNORM_BLOCK      : VK_FORMAT_R8G8_UNORM;    // 双通道：法线贴图的 R / G
    case 0x4C: return g_bcap & 32  ? VK_FORMAT_BC5_SNORM_BLOCK      : VK_FORMAT_R8G8_SNORM;
    case 0x4F: return g_bcap & 64  ? VK_FORMAT_BC6H_SFLOAT_BLOCK    : VK_FORMAT_R16G16B16A16_SFLOAT;   // 浮点亮度：RGBA16F，alpha 由解码器补 1.0
    case 0x50: return g_bcap & 64  ? VK_FORMAT_BC6H_UFLOAT_BLOCK    : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x4D: return g_bcap & 128 ? VK_FORMAT_BC7_UNORM_BLOCK      : VK_FORMAT_R8G8B8A8_UNORM;
    case 0x4E: return g_bcap & 128 ? VK_FORMAT_BC7_SRGB_BLOCK       : VK_FORMAT_R8G8B8A8_SRGB;
    case 0x79: return g_bcap & 256 ? VK_FORMAT_ASTC_4x4_UNORM_BLOCK  : 0;
    case 0x7A: return g_bcap & 256 ? VK_FORMAT_ASTC_5x4_UNORM_BLOCK  : 0;
    case 0x7B: return g_bcap & 256 ? VK_FORMAT_ASTC_5x5_UNORM_BLOCK  : 0;
    case 0x7C: return g_bcap & 256 ? VK_FORMAT_ASTC_6x5_UNORM_BLOCK  : 0;
    case 0x7D: return g_bcap & 256 ? VK_FORMAT_ASTC_6x6_UNORM_BLOCK  : 0;
    case 0x7E: return g_bcap & 256 ? VK_FORMAT_ASTC_8x5_UNORM_BLOCK  : 0;
    case 0x7F: return g_bcap & 256 ? VK_FORMAT_ASTC_8x6_UNORM_BLOCK  : 0;
    case 0x80: return g_bcap & 256 ? VK_FORMAT_ASTC_8x8_UNORM_BLOCK  : 0;
    case 0x81: return g_bcap & 256 ? VK_FORMAT_ASTC_10x5_UNORM_BLOCK : 0;
    case 0x82: return g_bcap & 256 ? VK_FORMAT_ASTC_10x6_UNORM_BLOCK : 0;
    case 0x83: return g_bcap & 256 ? VK_FORMAT_ASTC_10x8_UNORM_BLOCK : 0;
    case 0x84: return g_bcap & 256 ? VK_FORMAT_ASTC_10x10_UNORM_BLOCK : 0;
    case 0x85: return g_bcap & 256 ? VK_FORMAT_ASTC_12x10_UNORM_BLOCK : 0;
    case 0x86: return g_bcap & 256 ? VK_FORMAT_ASTC_12x12_UNORM_BLOCK : 0;
    case 0x87: return g_bcap & 256 ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK  : 0;
    case 0x88: return g_bcap & 256 ? VK_FORMAT_ASTC_5x4_SRGB_BLOCK  : 0;
    case 0x89: return g_bcap & 256 ? VK_FORMAT_ASTC_5x5_SRGB_BLOCK  : 0;
    case 0x8A: return g_bcap & 256 ? VK_FORMAT_ASTC_6x5_SRGB_BLOCK  : 0;
    case 0x8B: return g_bcap & 256 ? VK_FORMAT_ASTC_6x6_SRGB_BLOCK  : 0;
    case 0x8C: return g_bcap & 256 ? VK_FORMAT_ASTC_8x5_SRGB_BLOCK  : 0;
    case 0x8D: return g_bcap & 256 ? VK_FORMAT_ASTC_8x6_SRGB_BLOCK  : 0;
    case 0x8E: return g_bcap & 256 ? VK_FORMAT_ASTC_8x8_SRGB_BLOCK  : 0;
    case 0x8F: return g_bcap & 256 ? VK_FORMAT_ASTC_10x5_SRGB_BLOCK : 0;
    case 0x90: return g_bcap & 256 ? VK_FORMAT_ASTC_10x6_SRGB_BLOCK : 0;
    case 0x91: return g_bcap & 256 ? VK_FORMAT_ASTC_10x8_SRGB_BLOCK : 0;
    case 0x92: return g_bcap & 256 ? VK_FORMAT_ASTC_10x10_SRGB_BLOCK : 0;
    case 0x93: return g_bcap & 256 ? VK_FORMAT_ASTC_12x10_SRGB_BLOCK : 0;
    case 0x94: return g_bcap & 256 ? VK_FORMAT_ASTC_12x12_SRGB_BLOCK : 0;
    case 0x31: if (depth) *depth = 1; return VK_FORMAT_D24_UNORM_S8_UINT;    // STENCIL8：与深度共用一块，按 D24S8 挂 | shares storage with depth: attached as D24S8
    case 0x32: if (depth) *depth = 1; return VK_FORMAT_D16_UNORM;
    case 0x33: if (depth) *depth = 1; return VK_FORMAT_X8_D24_UNORM_PACK32;
    case 0x34: if (depth) *depth = 1; return VK_FORMAT_D32_SFLOAT;
    case 0x35: if (depth) *depth = 1; return VK_FORMAT_D24_UNORM_S8_UINT;
    case 0x36: if (depth) *depth = 1; return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default: return 0;
    }
}

static uint32_t mem_type(VkMemoryPropertyFlags need, VkMemoryPropertyFlags forbid)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mp.memoryTypes[i].propertyFlags & need) == need && !(mp.memoryTypes[i].propertyFlags & forbid)) return i;
    return 0xFFFFFFFFu;
}

static int find_type(VkMemoryRequirements rq, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((rq.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return (int)i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (rq.memoryTypeBits & (1u << i)) return (int)i;   // 退一步：能绑就行，属性由驱动定 | otherwise: take any bindable type and let the driver decide
    return -1;
}

// ---------------- 设备建立 | device setup ----------------
static int vk_boot(void)
{
    void *win; uint64_t w = 0, h = 0;
    if (!dm_ohos_window(&win, &w, &h)) return 0;
    const char *iext[] = { "VK_KHR_surface", "VK_OHOS_surface" };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "D2R-M2";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) { GRED("vkCreateInstance 失败"); return 0; }

    VkSurfaceCreateInfoOHOS sm = { VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS };
    sm.window = (OHNativeWindow *)win;
    if (vkCreateSurfaceOHOS(g_inst, &sm, NULL, &g_surf) != VK_SUCCESS) { GRED("vkCreateSurfaceOHOS 失败"); return 0; }

    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(g_inst, &npd, NULL);
    VkPhysicalDevice list[16];
    if (npd > 16) npd = 16;
    vkEnumeratePhysicalDevices(g_inst, &npd, list);
    for (uint32_t i = 0; i < npd && !g_pd; i++) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 sup = 0;
            vkGetPhysicalDeviceSurfaceSupportKHR(list[i], q, g_surf, &sup);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { g_pd = list[i]; g_qf = q; break; }
        }
    }
    if (!g_pd) { GRED("没有 graphics+present 的队列族"); return 0; }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(g_pd, &pp);
    GVL("[vk] 设备 %{public}s api %{public}u.%{public}u", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion));

    const char *dext[] = { "VK_KHR_swapchain", "VK_KHR_dynamic_rendering", "VK_KHR_maintenance4", "VK_KHR_create_renderpass2", "VK_KHR_timeline_semaphore" };
    // 只开实测在场的那些：装载器对没开的设备扩展会把驱动指针留成 NULL（M2-0 的 SIGABRT 就是这么来的）
    // Enable only the extensions actually present: for a device extension left off, the loader keeps the driver pointer NULL (that is exactly what aborted M2-0)
    uint32_t nd = 0;
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nd, NULL);
    VkExtensionProperties props[128];
    if (nd > 128) nd = 128;
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nd, props);
    const char *cand[8];
    uint32_t nc = 0;
    for (unsigned k = 0; k < sizeof dext / sizeof *dext && nc < 8; k++)
        for (uint32_t i = 0; i < nd; i++) if (!strcmp(props[i].extensionName, dext[k])) { cand[nc++] = dext[k]; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    // 各向异性：游戏采样器普遍声明 maxAnisotropy>1，特性没开就建采样器会直接失败（第 88 轮真实采样器那一刀要用）。
    // 只在实测支持时开这一条，其余特性维持原来的「全 0」。
    // Anisotropy: the game's samplers declare maxAnisotropy > 1 and vkCreateSampler faults when the feature is off (needed by the
    // round-88 real-sampler cut). Enabled only when the device actually reports it; every other feature stays off as before.
    VkPhysicalDeviceFeatures sup;
    vkGetPhysicalDeviceFeatures(g_pd, &sup);
    static VkPhysicalDeviceFeatures use;
    use = (VkPhysicalDeviceFeatures){ 0 };
    use.samplerAnisotropy = sup.samplerAnisotropy;
    dci.pEnabledFeatures = &use;
    // 一条条加：M2-0 只开 swapchain 建得起来，5 条一起开 vkCreateDevice 直接失败（第 35 轮），而驱动不点名是哪条。
    // 每加一条试建一次设备，加不上就踢掉并报名——建设备只在开机做，几次试错不要紧。
    // Add them one at a time: M2-0 built a device with swapchain alone, while asking for all five failed vkCreateDevice (round 35) without naming the culprit.
    // So try a device per added extension and drop-and-report the one that fails; creating a device happens once at boot, so the retries are free.
    const char *on[8];
    uint32_t nok = 0;
    for (uint32_t k = 0; k < nc; k++) {
        on[nok] = cand[k];
        dci.enabledExtensionCount = nok + 1;
        dci.ppEnabledExtensionNames = on;
        VkDevice d = NULL;
        if (vkCreateDevice(g_pd, &dci, NULL, &d) == VK_SUCCESS) { g_dev = d; nok++; continue; }
        GVL("[vk] 设备扩展踢掉 %{public}s（加上就建不起来）", cand[k]);
    }
    if (!g_dev) { GRED("vkCreateDevice 失败（第一条 %{public}s 都建不起来）", nc ? cand[0] : "无"); return 0; }
    GVL("[vk] 设备扩展用了 %{public}u / %{public}u 条（在场 %{public}u / 报出 %{public}u）", nok, nc, nd, (uint32_t)(sizeof dext / sizeof *dext));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g_qf;
    GK("vkCreateCommandPool", vkCreateCommandPool(g_dev, &cpi, NULL, &g_cp));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    GK("vkCreateFence", vkCreateFence(g_dev, &fci, NULL, &g_fence));
    GK("vkCreateFence(present)", vkCreateFence(g_dev, &fci, NULL, &g_pfence));
    g_mem_dev_local = mem_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    g_mem_host_visible = mem_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    GVL("[vk] 内存类型：deviceLocal=%{public}u hostVisibleCoherent=%{public}u 表面 %{public}u x %{public}u", g_mem_dev_local, g_mem_host_visible, (unsigned)w, (unsigned)h);
    return 1;
}

static void swapchain_destroy(void)
{
    if (!g_sc) return;
    vkQueueWaitIdle(g_q);
    for (uint32_t i = 0; i < g_sc_n; i++) {
        if (g_sc_fb[i]) { vkDestroyFramebuffer(g_dev, g_sc_fb[i], NULL); g_sc_fb[i] = NULL; }
        if (g_sc_view[i]) { vkDestroyImageView(g_dev, g_sc_view[i], NULL); g_sc_view[i] = NULL; }
        if (g_acq_sem[i]) { vkDestroySemaphore(g_dev, g_acq_sem[i], NULL); g_acq_sem[i] = NULL; }
        g_sc_img[i] = NULL;
    }
    if (g_sc_rp) { tri_invalidate(); vkDestroyRenderPass(g_dev, g_sc_rp, NULL); g_sc_rp = NULL; }
    if (g_off_fb) { vkDestroyFramebuffer(g_dev, g_off_fb, NULL); g_off_fb = NULL; }
    if (g_off_view) { vkDestroyImageView(g_dev, g_off_view, NULL); g_off_view = NULL; }
    if (g_off_img) { vkDestroyImage(g_dev, g_off_img, NULL); g_off_img = NULL; }
    if (g_off_mem) { vkFreeMemory(g_dev, g_off_mem, NULL); g_off_mem = NULL; }
    vkDestroySwapchainKHR(g_dev, g_sc, NULL);
    g_sc = NULL;
    g_sc_n = 0;
    g_sc_ready = 0;
}

static int swapchain_build(void *win, uint64_t ww, uint64_t hh)
{
    if (!g_surf) return 0;
    VkSurfaceCapabilitiesKHR cap;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &cap) != VK_SUCCESS) { GRED("拿不到表面能力"); return 0; }
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fm[16];
    if (nf > 16) nf = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fm);
    // 面板实测唯一可用合成 alpha = INHERIT、变换 ROTATE_90；格式优先选与窗口贴图同族，减少 blit 的格式限制
    // Measured panel: only compositeAlpha INHERIT, currentTransform ROTATE_90. Prefer the format in the window texture's family so blits stay format-legal
    GVL("[vk] 表面能力 %{public}u x %{public}u｜supportedTransforms=0x%{public}x currentTransform=0x%{public}x（位：1=IDENTITY 2=ROTATE_90 8=ROTATE_270）",
        cap.currentExtent.width, cap.currentExtent.height, (unsigned)cap.supportedTransforms, (unsigned)cap.currentTransform);
    g_sc_fmt = fm[0].format;
    int pick = -1;
    for (uint32_t i = 0; i < nf; i++)
        if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { pick = (int)i; break; }
    if (pick >= 0) g_sc_fmt = fm[pick].format;
    VkExtent2D ext = cap.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = (uint32_t)ww; ext.height = (uint32_t)hh; }
    uint32_t n = cap.minImageCount + 1;
    if (cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount;
    if (n > 8) n = 8;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = g_surf;
    sci.minImageCount = n;
    sci.imageFormat = g_sc_fmt;
    sci.imageColorSpace = fm[pick >= 0 ? pick : 0].colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;   // 冒烟实测的用法位；加 TRANSFER_DST 也不让写（第 47 轮：对交换链图做 transfer = 打掉设备）
                                                            // The usage bits the smoke test used; TRANSFER_DST is not honoured either (round 47)
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // 第 110 轮实测（B33 截图）：声明 preTransform=ROTATE_90 而内容一点没转 ⇒ 合成器只把游戏这一层转了 90°，
    //   ArkUI 的覆盖字是正的，游戏 UI 全体横倒（世界大小那排「无限/大量/中等」要逆时针转 90 才能读）。
    //   合成器按 preTransform 的**逆**变换我们这层，所以「声明转、内容不转」= 屏幕上真转了 90°。
    //   改法：只要驱动允许（supportedTransforms 位里有点 IDENTITY），就声明不转 —— 内容本来已经是正的。
    //   不查这一位就硬试 IDENTITY 不行：建链失败可能连带把表面打掉（acquire 直接 SURFACE_LOST）。
    // Round 110 (B33 screenshots): we declared preTransform=ROTATE_90 but never pre-rotated the content, so the compositor
    // rotated only the game layer - the ArkUI overlay stayed upright while the whole UI came out sideways. The compositor
    // applies the *inverse* of preTransform, so "declare a rotation, draw none" is exactly a 90-degree turn on screen.
    // Take IDENTITY when the driver offers it; probing without checking supportedTransforms risks killing the surface.
    sci.preTransform = cap.currentTransform;
    if (cap.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
        (cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
        sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        GVL("[vk] 交换链 preTransform 用 IDENTITY（面板 currentTransform=%{public}d，合成器不再转这层）", (int)cap.currentTransform);
    }
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_dev, &sci, NULL, &g_sc) != VK_SUCCESS) { GRED("vkCreateSwapchainKHR 失败"); return 0; }
    vkGetSwapchainImagesKHR(g_dev, g_sc, &g_sc_n, NULL);
    if (g_sc_n > 8) g_sc_n = 8;
    vkGetSwapchainImagesKHR(g_dev, g_sc, &g_sc_n, g_sc_img);
    for (uint32_t i = 0; i < g_sc_n; i++) {
        VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image = g_sc_img[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = g_sc_fmt;
        iv.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        GK("vkCreateImageView(swap)", vkCreateImageView(g_dev, &iv, NULL, &g_sc_view[i]));
        VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        GK("vkCreateSemaphore", vkCreateSemaphore(g_dev, &s, NULL, &g_acq_sem[i]));
    }
    // 出图通道：抄 dm/ohos/vk_smoke.c:318-359 的实测绿写法（经典 render pass + 每张一个 framebuffer）
    // The present pass copies vk_smoke.c:318-359, the recipe that measured green on this device.
    {
        VkAttachmentDescription a = {0};
        a.format = g_sc_fmt;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp = {0};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ar;
        VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            0, 0, VK_DEPENDENCY_BY_REGION_BIT };
        VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpc.attachmentCount = 1; rpc.pAttachments = &a;
        rpc.subpassCount = 1; rpc.pSubpasses = &sp;
        rpc.dependencyCount = 1; rpc.pDependencies = &dep;
        GK("vkCreateRenderPass(swap)", vkCreateRenderPass(g_dev, &rpc, NULL, &g_sc_rp));
        for (uint32_t i = 0; i < g_sc_n; i++) {
            VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fbi.renderPass = g_sc_rp;
            fbi.attachmentCount = 1; fbi.pAttachments = &g_sc_view[i];
            fbi.width = ext.width; fbi.height = ext.height; fbi.layers = 1;
            GK("vkCreateFramebuffer(swap)", vkCreateFramebuffer(g_dev, &fbi, NULL, &g_sc_fb[i]));
        }
    }
    if (!g_vis_sem) { VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        GK("vkCreateSemaphore(vis)", vkCreateSemaphore(g_dev, &s, NULL, &g_vis_sem)); }
    // 位 512 的对照现场：同尺寸同格式的一张普通 VkImage，通道写它、交换链那张完全不碰。
    // 用来把「写交换链图」和「经典通道本身」这两个嫌疑分开——游戏的通道已经全改成经典（第 58 轮）仍然 35 帧就 present = -4。
    // The control target for bit 512: a plain VkImage of the same size and format, written by the pass while the swapchain image is left
    // alone. This separates "writing a swapchain image" from "a classic pass exists at all" - the game's passes are classic since round 58
    // and present still answered -4 after 35 frames.
    if (!g_off_img) {
        VkImageCreateInfo oi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        oi.imageType = VK_IMAGE_TYPE_2D;
        oi.format = g_sc_fmt;
        oi.extent = (VkExtent3D){ ext.width, ext.height, 1 };
        oi.mipLevels = 1; oi.arrayLayers = 1;
        oi.samples = VK_SAMPLE_COUNT_1_BIT;
        oi.tiling = VK_IMAGE_TILING_OPTIMAL;
        oi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        oi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g_dev, &oi, NULL, &g_off_img) == VK_SUCCESS) {
            VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, g_off_img, &rq);
            int ti = find_type(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = rq.size; ai.memoryTypeIndex = ti >= 0 ? (uint32_t)ti : 0;
            if (ti >= 0 && vkAllocateMemory(g_dev, &ai, NULL, &g_off_mem) == VK_SUCCESS) {
                vkBindImageMemory(g_dev, g_off_img, g_off_mem, 0);
                VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                iv.image = g_off_img; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = g_sc_fmt;
                iv.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                GK("vkCreateImageView(off)", vkCreateImageView(g_dev, &iv, NULL, &g_off_view));
                VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                fbi.renderPass = g_sc_rp; fbi.attachmentCount = 1; fbi.pAttachments = &g_off_view;
                fbi.width = ext.width; fbi.height = ext.height; fbi.layers = 1;
                GK("vkCreateFramebuffer(off)", vkCreateFramebuffer(g_dev, &fbi, NULL, &g_off_fb));
            }
        }
    }
    g_sc_ext = ext;
    g_sc_ready = 1;
    GVL("[vk] 交换链 %{public}u 张 %{public}u x %{public}u 格式 %{public}d", g_sc_n, ext.width, ext.height, (int)g_sc_fmt);
    return 1;
}

// ---------------- 出图管线：满屏三角形采样源图（M2-2）| present pipeline: full-screen triangle sampling the source ----------------
// 管线只能建在经典 VkRenderPass 上（第 48 轮实测：动态渲染挂交换链视图＝GPU 炸），所以复用 g_sc_rp；
// 交换链重建（present 报 OUT_OF_DATE）时 g_sc_rp 换新，管线与帧缓冲一起作废，下一帧懒建。
// A pipeline must be built against a classic VkRenderPass (round 48: dynamic rendering onto a swapchain view faults the GPU), so this
// reuses g_sc_rp. Rebuilding the swapchain replaces g_sc_rp, which invalidates the pipeline - it comes back lazily next frame.
static VkShaderModule g_tri_vs, g_tri_fs;
static VkPipelineLayout g_tri_pl;
static VkPipeline g_tri_pipe;
static VkRenderPass g_tri_rp;              // 管线是照哪条通道建的（与 g_sc_rp 不一致就重造）| the render pass the pipeline was built against
static VkSampler g_tri_samp;
static VkDescriptorSetLayout g_tri_dsl;
static VkDescriptorPool g_tri_pool;
typedef struct { uint64_t serial; VkDescriptorSet set; } trisets;
#define TRISET_N 16
static trisets g_tri_set[TRISET_N];
static uint64_t g_tri_drawn;
static int g_tri_fail;                      // 建一次失败就不再反复试（每次失败都已报出返回值）| one creation failure stops further attempts - each one already printed its result

// 交换链重建 = g_sc_rp 换新：管线、以及缓存的描述符集（视图可能跟着换）一起作废，下一帧懒建
// A swapchain rebuild replaces g_sc_rp, so the pipeline and the cached descriptor sets (whose views may be gone too) are dropped - they come back lazily.
static void tri_invalidate(void)
{
    if (g_tri_pipe) { vkDestroyPipeline(g_dev, g_tri_pipe, NULL); g_tri_pipe = NULL; }
    g_tri_rp = NULL;
    g_tri_fail = 0;
    for (int i = 0; i < TRISET_N; i++) g_tri_set[i].serial = 0;   // 池与集本身留着，下次直接覆写 | the pool and sets stay; entries are just rewritten
}

static int tri_module(const uint32_t *code, uint32_t words, VkShaderModule *out, const char *who)
{
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = (size_t)words * 4;
    mi.pCode = code;
    VkResult r = vkCreateShaderModule(g_dev, &mi, NULL, out);
    if (r != VK_SUCCESS) { GRED("建 %{public}s 着色模块 = %{public}d (%{public}s)｜字数 %{public}u 头 %{public}#x", who, (int)r, vr(r), words, code[0]); return 0; }
    return 1;
}

static int tri_boot(void)
{
    if (g_tri_pipe || g_tri_fail) return g_tri_pipe ? 1 : 0;
    if (!g_sc_rp) return 0;
    if (!g_tri_samp) {
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod = 0; si.maxLod = 0;                // 现在还没做 mip 上传，只读 0 级 | mip upload comes later: read level 0 only
        if (vkCreateSampler(g_dev, &si, NULL, &g_tri_samp) != VK_SUCCESS) { GRED("建采样器失败"); g_tri_fail = 1; return 0; }
    }
    if (!g_tri_dsl) {
        VkDescriptorSetLayoutBinding b = {0};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo di = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        di.bindingCount = 1; di.pBindings = &b;
        if (vkCreateDescriptorSetLayout(g_dev, &di, NULL, &g_tri_dsl) != VK_SUCCESS) { GRED("建描述符布局失败"); g_tri_fail = 1; return 0; }
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TRISET_N };
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = TRISET_N; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(g_dev, &pi, NULL, &g_tri_pool) != VK_SUCCESS) { GRED("建描述符池失败"); g_tri_fail = 1; return 0; }
    }
    if (!g_tri_pl) {
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        li.setLayoutCount = 1; li.pSetLayouts = &g_tri_dsl;
        if (vkCreatePipelineLayout(g_dev, &li, NULL, &g_tri_pl) != VK_SUCCESS) { GRED("建管线布局失败"); g_tri_fail = 1; return 0; }
    }
    if (!g_tri_vs && !tri_module(sh_tri_vert, sh_tri_vert_len, &g_tri_vs, "顶点")) { g_tri_fail = 1; return 0; }
    if (!g_tri_fs && !tri_module(sh_tri_frag, sh_tri_frag_len, &g_tri_fs, "像素")) { g_tri_fail = 1; return 0; }
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_tri_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_tri_fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    // 顶点全部由 gl_VertexIndex 生成，没有顶点流 | every vertex comes from gl_VertexIndex - no vertex stream
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, (float)g_sc_ext.width, (float)g_sc_ext.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, g_sc_ext };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vs.viewportCount = 1; vs.pViewports = &vp; vs.scissorCount = 1; vs.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {0};
    // 只写 RGB、alpha 留通道清出来的 1.0：出图那张的 alpha 若是 0，预乘合成会把整张表面抹成透明（坑 36），
    // 于是「画上了」和「没画上」在截图上长得一样。alpha 恒为 1 之后，屏幕颜色就等于源图的 RGB。
    // Write RGB only and keep the alpha the pass cleared to 1: a source whose alpha is 0 makes the whole surface
    // transparent under premultiplied compositing (pitfall 36), so "drew" and "did not draw" look identical in a
    // screenshot. With alpha pinned at 1 the screen color is exactly the source RGB.
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vs;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb;
    gi.pDynamicState = &ds;
    gi.layout = g_tri_pl;                    // 漏了这条 = 管线布局 NULL：Maleoon 在 vkCreateGraphicsPipelines 里直接解引用空指针（第 62 轮 SIGSEGV@0xa8，栈顶 libmaleoon 八层）
                                             // Missing this makes the layout NULL and the driver dereferences it inside vkCreateGraphicsPipelines (round 62: SIGSEGV@0xa8, eight driver frames).
    gi.renderPass = g_sc_rp; gi.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(g_dev, NULL, 1, &gi, NULL, &g_tri_pipe);
    if (r != VK_SUCCESS) { GRED("建满屏三角形管线 = %{public}d (%{public}s)｜通道 %{public}p 格式 %{public}d 尺寸 %{public}u x %{public}u",
                                 (int)r, vr(r), (void *)(uintptr_t)g_sc_rp, (int)g_sc_fmt, g_sc_ext.width, g_sc_ext.height);
                           g_tri_fail = 1; return 0; }
    g_tri_rp = g_sc_rp;
    GVL("[vk] 满屏三角形管线就绪（通道 %{public}p、着色模块 顶 %{public}u 字 / 像 %{public}u 字）",
        (void *)(uintptr_t)g_tri_rp, sh_tri_vert_len, sh_tri_frag_len);
    return 1;
}

// 源图 → 采样用的描述符集（按 serial 缓存，图换了就换一条）| source texture -> its descriptor set (cached per serial)
static VkDescriptorSet tri_set_for(vktex *v)
{
    for (int i = 0; i < TRISET_N; i++) if (g_tri_set[i].serial == v->serial) return g_tri_set[i].set;
    int slot = -1;
    for (int i = 0; i < TRISET_N; i++) if (!g_tri_set[i].serial) { slot = i; break; }
    if (slot < 0) { static int saidfull; if (!saidfull++) GRED("满屏三角形描述符集缓存满了（%{public}d 条）", TRISET_N); return NULL; }
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_tri_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_tri_dsl;
    if (vkAllocateDescriptorSets(g_dev, &ai, &g_tri_set[slot].set) != VK_SUCCESS) { GRED("分配满屏三角形描述符集失败"); return NULL; }
    g_tri_set[slot].serial = v->serial;
    VkDescriptorImageInfo ii = {0};
    ii.sampler = g_tri_samp; ii.imageView = v->view; ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = g_tri_set[slot].set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    return g_tri_set[slot].set;
}

// 通道**外**的准备：源图转成可采样布局 + 取描述符集，返回 0 表示这一帧不画（位 1024、深度图、管线没建起来）
// 布局转换必须在 BeginRenderPass 之前发：通道里只允许转换本帧缓冲的附件，源图不是附件（校验层不在，规矩得自己守）
// Preparation outside the pass: put the source into a sampleable layout and get its descriptor set; NULL means no draw this frame
// (bit 1024, a depth image, or the pipeline did not come up). The layout transition has to be issued before BeginRenderPass - inside a
// pass a barrier may only transition attachments of that framebuffer, and the source is not one of them. No validation layer, so we keep the rule ourselves.
static VkDescriptorSet tri_prep(VkCommandBuffer cb, vktex *v)
{
    if (g_skip & 1024) { static int s1024; if (!s1024++) GVL("[vk] DM_VK_SKIP 位 1024：出图只清色，不画三角形"); return NULL; }
    if (v->is_depth || !v->view) return NULL;
    if (!tri_boot()) return NULL;
    if (g_tri_rp != g_sc_rp) return NULL;      // 交换链重建过：管线跟着作废，这一帧先只清色 | swapchain was rebuilt - the pipeline comes back next frame
    VkDescriptorSet set = tri_set_for(v);
    if (!set) return NULL;
    // 源图可能被游戏的通道留在 COLOR_ATTACHMENT；采样要 SHADER_READ_ONLY
    // The game may have left the source in COLOR_ATTACHMENT; sampling wants SHADER_READ_ONLY.
    to_layout(cb, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return set;
}

// 通道**内**：绑管线 + 画一个满屏三角形，把源图贴满整张
// Inside the pass: bind and draw the one full-screen triangle that covers the whole image.
static void tri_emit(VkCommandBuffer cb, VkDescriptorSet set)
{
    VkViewport vp = { 0, 0, (float)g_sc_ext.width, (float)g_sc_ext.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, g_sc_ext };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_tri_pipe);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_tri_pl, 0, 1, &set, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0);
    g_tri_drawn++;
}

// ---------------- 贴图：serial → VkImage | textures ----------------
// 同一块池内存上的新生命：显存里那份像素还是上一世的。写保护缺页在这台机器上一轮只响 2 次，脏检查指望不上，
// 所以按「游戏既然在同一块字节上重新 Initialize，就是往里写了新东西」处理。
// 只对纯 CPU 供图的打包贴图成立：渲染目标（当过附件）的池字节是分配器垃圾，绝不能倒灌回去。
// 这里只打标记、不排队：排队要存指针，而 g_tex 是数组、释放时按末尾换位，攒到通道外冲刷时指针可能已经指向别张（甚至空槽）。
// 位 536870912 把这条整段关掉，同一构建可做 A/B。
// A new life over the same pool bytes: the device copy is still the previous life's. The write-protect fault fires twice a round on this
// device, so the dirty check cannot be relied on - treat a re-Initialize as "the game wrote new pixels here".
// Only for CPU-sourced packaged textures: a render target's pool bytes are allocator garbage and must never be copied back.
// This only raises the flag - queueing would store a pointer into an array that swap-removes on release, so it could point at another
// texture (or a cleared slot) by the time the copy is issued. Bit 536870912 switches the whole thing off for an A/B in one build.
static void tex_new_life(vktex *v)
{
    if (g_skip & 536870912 || !v->uploaded || v->drawn) return;
    v->pkg_stale = 1;
    g_tex_stale++;
}

static vktex *tex_get(const tex_t *t)
{
    if (!t || !t->serial) return NULL;
    for (int i = 0; i < g_tex_n; i++) if (g_tex[i].serial == t->serial && memcmp(&g_tex[i].d.d, &t->d, sizeof t->d) == 0) { g_tex_hit++; return &g_tex[i]; }
    // 身份第二查：池 + 偏移 + 描述符相同 = 同一块字节的同一张图，serial 换了也算它。
    // 游戏每帧在同一块池内存上重新 Initialize 这张图（第 112 轮的每帧 serial 942→968），按 serial 认就每帧多建一张空图。
    // Second identity check: same pool + offset + descriptor is the same bytes, whatever the serial says. The game re-initializes the
    // same pool storage every frame, so a serial-only lookup builds a fresh empty image each frame.
    if (!(g_skip & 65536)) for (int i = 0; i < g_tex_n; i++) if (g_tex[i].img && tex_same_store(&g_tex[i], t)) {
        g_tex[i].serial = t->serial; g_tex[i].d = *t;
        g_tex[i].sibchk = g_tex[i].sib = 0;
        tex_new_life(&g_tex[i]);
        g_tex_stor++; g_tex_hit++;
        return &g_tex[i];
    }
    // 回收站：这张上一世刚被 Finalize，像素还在那块显存里，直接复活比空着一张重来强。
    // The recycle bin: this image's previous life was just finalized and its pixels are still in that allocation - revive it.
    if (!(g_skip & 65536)) for (int i = 0; i < g_recy_n; i++) if (tex_same_store(&g_recy[i], t) && g_tex_n < VTN) {
        vktex *v = &g_tex[g_tex_n++];
        *v = g_recy[i];
        g_recy_bytes -= v->vk_bytes;
        g_recy[i] = g_recy[--g_recy_n];
        memset(&g_recy[g_recy_n], 0, sizeof g_recy[g_recy_n]);
        v->serial = t->serial; v->d = *t; v->recy_seq = 0; v->sibchk = v->sib = 0;
        tex_new_life(v);
        g_tex_revive++;
        g_tex_gen++; g_sets_valid = 0;   // 缓存的集子里这一槽还指着哑图：作废，下一笔重新绑真视图
        return v;
    }
    // 同一个 serial 建出第二张 VkImage = 描述符没对上：游戏往 A 画、出图采的是 B，B 永远全零。
    // 第 94 轮实测「出图源挂通道 0 次、读回 alpha 全 0」，这是头号嫌疑，先把撞 serial 的次数打出来。
    // A second VkImage under one NVN serial means the descriptor compare missed: the game draws into A while the present samples an
    // untouched B. Round 94 measured exactly that signature (present source never attached to a pass, readback all zeros), so count it.
    for (int i = 0; i < g_tex_n; i++) if (g_tex[i].serial == t->serial) {
        g_tex_dupser++;
        if (g_tex_dupser <= 8) {
            char rb[224];
            snprintf(rb, sizeof rb, "[vk] 同 serial 二次建图 serial=%llu 第%llu 次 旧 格%d %ux%ux%u 层%d 级%d｜新 格%d %ux%ux%u 层%d 级%d",
                     (unsigned long long)t->serial, (unsigned long long)g_tex_dupser,
                     g_tex[i].d.d.format, g_tex[i].d.d.width, g_tex[i].d.d.height, g_tex[i].d.d.depth,
                     g_tex[i].d.L.layers, g_tex[i].d.d.levels,
                     t->d.format, t->d.width, t->d.height, t->d.depth, t->L.layers, t->d.levels);
            GVL("%{public}s", rb);
        }
        break;
    }
    int depth = 0;
    // 同池同偏移、描述符却对不上 = 身份判定漏了一张：游戏在同一块字节上重新 Initialize，某一字段变了，
    // 于是新建一张空图。旧的那张还在被画、新的这张被采，正是闪屏的形状。把到底哪一字段不一致点名打出来。
    // Same pool and offset but a descriptor that disagrees means the identity test missed one: the game re-initialized over the same
    // bytes, a field changed, so a fresh empty image appears while the draws still land on the old one - exactly the flicker's shape.
    // Print which field it is.
    if (!(g_skip & 65536) && t->pool) {
        for (int i = 0; i < g_tex_n + g_recy_n; i++) {
            const tex_t *o = i < g_tex_n ? &g_tex[i].d : &g_recy[i - g_tex_n].d;
            if (o->pool != t->pool || o->off != t->off) continue;
            const nvn_texdesc *a = &o->d, *b = &t->d;
            g_tex_stormis++;
            if (g_tex_stormis <= 10)
                GVL("[vk] 同池偏移漏判 第%llu 次 s%llu→%llu %ux%u 格%d｜变的是 目%d 格%d 旗%d 采%d 宽%d 高%d 深%d 级%d 跨%d 块高%d 层%d",
                    (unsigned long long)g_tex_stormis, (unsigned long long)o->serial, (unsigned long long)t->serial,
                    b->width, b->height, b->format,
                    a->target != b->target, a->format != b->format, a->flags != b->flags, a->samples != b->samples,
                    a->width != b->width, a->height != b->height, a->depth != b->depth, a->levels != b->levels,
                    a->stride != b->stride, a->pkg_block_h != b->pkg_block_h, o->L.layers != t->L.layers);
            break;
        }
    }
    VkFormat f = fmt_of(t->d.format, &depth);
    if (!f) { if (++g_tex_noformat <= 6) GRED("认不出的 NVN 格式 %{public}d（%{public}u x %{public}u），这张先不建", t->d.format, t->d.width, t->d.height); return NULL; }
    if (g_tex_n >= VTN) { GRED("贴图表满了（%{public}d）", VTN); return NULL; }
    vktex *v = &g_tex[g_tex_n++];
    g_tex_gen++;
    g_sets_valid = 0;   // 缓存的绑定里这张还是哑图：新建一张就把集子作废，下一次绘制重新绑上真视图
                        // A cached set still has the dummy for this texture: drop the cache so the next draw rebinds the real view
    v->serial = t->serial;
    v->d = *t;
    v->fmt = f;
    v->soft_bc = (t->d.format >= 0x41 && t->d.format <= 0x50) &&
                 !(f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    v->is_depth = depth;
    v->w = t->d.width ? t->d.width : 1;
    v->h = t->d.height ? t->d.height : 1;
    v->layers = t->L.layers ? t->L.layers : 1;
    v->levels = t->d.levels ? t->d.levels : 1;
    for (int i = 0; i < 24; i++) {   // 格式直方图（第 82 轮只扫 10 格，BC 一多就把后面的格式吞了）
        if (g_txfmt[i].f == t->d.format && g_txfmt[i].n) { atomic_fetch_add(&g_txfmt[i].n, 1); break; }
        if (!g_txfmt[i].n) { g_txfmt[i].f = t->d.format; atomic_fetch_add(&g_txfmt[i].n, 1); break; }
    }

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = f;
    ii.extent = (VkExtent3D){ v->w, v->h, t->d.depth > 1 ? t->d.depth : 1 };
    if (t->d.depth > 1) v->layers = t->d.depth;
    ii.mipLevels = v->levels;
    ii.arrayLayers = v->layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    // 深度图也要可采样：游戏的阴影图就是一趟深度通道写完、下一趟用比较采样器读回来。少了 SAMPLED 位，
    // 那次读只能落回 1x1 哑图（第 88 轮画面全白的一条来源）。多了这位的代价是马良可能给深度图换一种内存布局，
    // 实测过才算数。
    // Depth images must be sampleable too: the game's shadow map is written by one depth pass and read back by the next through a
    // comparison sampler. Without SAMPLED that read can only fall back to the 1x1 dummy (one source of round 88's all-white frame).
    ii.usage = depth ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                     : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (v->layers > 1) ii.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT_KHR;
    if (vkCreateImage(g_dev, &ii, NULL, &v->img) != VK_SUCCESS) { GRED("vkCreateImage 失败 %{public}u x %{public}u 格式 %{public}d", v->w, v->h, (int)f); v->img = NULL; return NULL; }
    VkMemoryRequirements rq;
    vkGetImageMemoryRequirements(g_dev, v->img, &rq);
    int ti = find_type(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ti < 0) { GRED("没有可绑的内存类型"); vkDestroyImage(g_dev, v->img, NULL); v->img = NULL; return NULL; }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = (uint32_t)ti;
    if (vkAllocateMemory(g_dev, &ai, NULL, &v->mem) != VK_SUCCESS) { GRED("vkAllocateMemory 失败 %{public}llu", (unsigned long long)rq.size); vkDestroyImage(g_dev, v->img, NULL); v->img = NULL; return NULL; }
    vkBindImageMemory(g_dev, v->img, v->mem, 0);
    v->vk_bytes = rq.size;             // 回收站按这个数算预算 | the recycle budget counts these bytes
    VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    iv.image = v->img;
    // NVN 的 target 编号（nvn.h:778）：1=2D 2=3D 3=1D阵列 4=2D阵列 8=CUBE 9=CUBE阵列
    // NVN target numbering (nvn.h:778): 1=2D 2=3D 3=1D array 4=2D array 8=cube 9=cube array
    int tg = v->d.d.target;
    iv.viewType = (tg == 4 || tg == 9 || tg == 3) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                : tg == 8 ? VK_IMAGE_VIEW_TYPE_CUBE
                : tg == 2 ? VK_IMAGE_VIEW_TYPE_3D
                : VK_IMAGE_VIEW_TYPE_2D;
    iv.format = f;
    // 少于四分量时把 A 显式改写成 ONE：NVN（GL 规矩）让着色器采到 A=1.0，而 Vulkan 的恒等映射只在格式含
    // 归一化分量时才补 1.0——R11G11B10F、RGB9E5F、R32F 这类半浮/整型格式给出的是 A=0，UI 层用的正是这些；
    // 另外 RGBX/BGRX 在 Vulkan 里只能建成四通道格式，第四字节是客体没写的垃圾，必须让 A 走 ONE 而不是读它。
    // A missing alpha must be spelled out: NVN (GL rules) samples such texels as A=1.0, while Vulkan's identity swizzle
    // substitutes 1.0 only for formats with a scaled component - half-float/int packeds give 0.0 - and RGBX/BGRX can only
    // be rebuilt as 4-channel views whose fourth byte is unwritten garbage.
    {
        int ch = nvc_ch((int)t->d.format);
        if (ch == 1 || ch == 2 || ch == 3) {
            iv.components.r = VK_COMPONENT_SWIZZLE_R;
            iv.components.g = ch >= 2 ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_ZERO;
            iv.components.b = ch >= 3 ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_ZERO;
            iv.components.a = VK_COMPONENT_SWIZZLE_ONE;
            if (g_tex_new < 8) GVL("[vk] 视图补 A=1：NVN 格式 %{public}d 分量 %{public}d → Vk %{public}d", (int)t->d.format, ch, (int)f);
        }
    }
    iv.subresourceRange = (VkImageSubresourceRange){ depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, v->levels, 0, v->layers };
    GK("vkCreateImageView", vkCreateImageView(g_dev, &iv, NULL, &v->view));
    v->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_tex_new++;
    if (g_tex_new <= 8) GVL("[vk] 建贴图 serial=%{public}llu %{public}u x %{public}u x%{public}u 格式 NVN %{public}d → Vk %{public}d（第 %{public}llu 张）",
                            (unsigned long long)t->serial, v->w, v->h, v->layers, t->d.format, (int)f, (unsigned long long)g_tex_new);
    return v;
}

// 只按 serial 查（不建）：探针与描述符填写用 | lookup by serial only, for probes and descriptor fills
static vktex *tex_of_serial(uint64_t s)
{
    if (!s) return NULL;
    if (g_tex_idx_gen != g_tex_gen) tex_idx_build();
    unsigned q = tex_hash(s);
    for (int t = 0; t < (int)(sizeof g_tex_idx / sizeof g_tex_idx[0]); t++) {
        int e = g_tex_idx[(q + (unsigned)t) & 2047u];
        if (!e) return NULL;
        if (g_tex[e - 1].serial == s) return &g_tex[e - 1];
    }
    return NULL;
}

// tid → 贴图登记（照抄 nvn_mtl.m:2145 那一套，那边 D2R 已经画得出来）：
// NVN 的贴图句柄是「贴图池槽号」，着色器绑定里只有这个号，所以要先按号拿到那张 NVN 贴图的序号，再按序号找 VkImage。
// 登记时按值存一份快照：游戏会按值拷贝 NVN 对象，句柄后面那块对象内存可能被复用，指针存下来不可靠。
// tid -> texture registration, copied from nvn_mtl.m:2145 (that path renders D2R): an NVN texture handle is a slot number in the
// texture pool, and a shader binding carries only that number, so the layer needs handle -> guest serial -> VkImage.
// The registration stores a by-value snapshot - the game copies NVN objects by value, so the object memory behind a handle can be reused.
typedef struct { tex_t t; view_t v; uint64_t serial; uint8_t has_v, ok; } treg_t;
#define TREG_MAX 65536                 // NVN 贴图池槽位上限：超出就是脏句柄，不跟着 realloc（否则一个坏号能要 8 MB 表）| bogus handles above this are dropped instead of growing the table
static treg_t *g_treg; static int g_ntreg;
static pthread_mutex_t g_regm = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_tex_reg, g_tex_reg_big, g_tex_rereg;

void mtl_reg_tex(int tid, const tex_t *t, const view_t *v)
{
    // 不看 g_on：登记只是记账，没必要等 Vulkan 层起来。第 85 轮带着这道门记到 9937 条，仍出现「槽 8 句柄 701 查无此图」，
    // 说明至少有一部分登记发生在层起来之前（下一轮看 登记 是否明显变大即可证实）。
    // Not gated on g_on: registration is bookkeeping and need not wait for the layer. Round 85 recorded 9937 registrations behind that gate
    // and still hit 「slot 8 handle 701 with nothing registered」, so some registrations must predate the layer - the next round's 登记
    // count either confirms it or rules it out.
    if (!t || !t->serial) return;
    if (tid < 0 || tid >= TREG_MAX) { atomic_fetch_add(&g_tex_reg_big, 1); return; }
    pthread_mutex_lock(&g_regm);
    if (tid >= g_ntreg) {
        int want = (tid / 1024 + 1) * 1024;
        treg_t *nw = (treg_t *)realloc(g_treg, (size_t)want * sizeof *nw);
        if (!nw) { pthread_mutex_unlock(&g_regm); return; }
        memset(nw + g_ntreg, 0, (size_t)(want - g_ntreg) * sizeof *nw);
        g_treg = nw; g_ntreg = want;
    }
    treg_t *e = &g_treg[tid];
    // 同一个槽位换了身份：游戏把这张 NVNtexture 重新初始化了（UI 贴图每换一次布局就会），
    // 而我们按注册时的 serial 快照建图——采样侧就会永远停在一张空旧图上。先数出来再说怎么跟。
    // The same slot changed identity: the game re-initialized this texture (UI targets do that on every relayout) while our
    // snapshot keys the image made at registration time - the sample side would then be pinned to a stale, empty image.
    if (e->ok && e->serial && t->serial != e->serial) {
        atomic_fetch_add(&g_tex_rereg, 1);
        if (atomic_load(&g_tex_rereg) <= 10)
            GVL("[vk] 同槽换图 tid=%{public}d 旧 serial=%{public}llu → 新 serial=%{public}llu（旧 %{public}u x %{public}u 新 %{public}u x %{public}u 格式%{public}d→%{public}d）",
                tid, (unsigned long long)e->serial, (unsigned long long)t->serial, e->t.d.width, e->t.d.height,
                t->d.width, t->d.height, e->t.d.format, t->d.format);
    }
    e->t = *t; e->serial = t->serial;
    if (v) { e->v = *v; e->has_v = 1; } else e->has_v = 0;
    e->ok = 1;
    atomic_fetch_add(&g_tex_reg, 1);
    pthread_mutex_unlock(&g_regm);
}

// 句柄 → 已建成的 Vk 视图（不建图：描述符这条路绝不顺手造 VkImage，一个菜单能登记上千张贴图）
// Handle -> an existing view. This path never creates: one menu registers thousands of textures, and creating them all from
// the descriptors would exhaust the device.
static vktex *tex_of_tid(int tid)
{
    uint64_t ser = 0;
    if (tid <= 0 || tid >= TREG_MAX) return NULL;
    pthread_mutex_lock(&g_regm);
    if (g_treg && tid < g_ntreg && g_treg[tid].ok) ser = g_treg[tid].serial;
    pthread_mutex_unlock(&g_regm);
    return ser ? tex_of_serial(ser) : NULL;
}

// 按需建图：句柄查得到描述符、但从没建过 VkImage。以前只在「上传」那一路建图，所以游戏拿一张图当渲染目标、
// 下一趟再采样时，绑定这一路看到的永远是哑图（第 92 轮实测「没图」占哑图大头 757513 / 794833）。
// 上限是因为贴图表一共 VTN=4096 格，得给上传那条路留位；位 32768 关掉它做 A/B。
// Create-on-demand: the handle resolves to a descriptor but no VkImage was ever built, because images used to appear only on the
// upload path - so a target the game renders into and samples one pass later always bound a dummy (round 92: 757513 of 794833 dummies
// were 「没图」). Capped because the texture table has VTN=4096 slots and the upload path needs its share; bit 32768 disables it.
static _Atomic uint64_t g_tex_make, g_tex_make_full;
#define TEX_MAKE_MAX 3000
#define TEX_MAKE_MAX_HI 12000          // 位 131072 抬到这个数，做内存 A/B | bit 131072 raises the cap - the memory A/B
static vktex *tex_make_tid(int tid)
{
    tex_t t; int have = 0;
    if (tid <= 0 || tid >= TREG_MAX) return NULL;
    pthread_mutex_lock(&g_regm);
    if (g_treg && tid < g_ntreg && g_treg[tid].ok) { t = g_treg[tid].t; have = 1; }
    pthread_mutex_unlock(&g_regm);
    if (!have || !t.serial) return NULL;
    // 第 96 轮：上限数的是「调用次数」，一张早就建好的图每帧重绑一次也照扣，扣满 3000 之后连现成的图都不给了
    //   ⇒ 撞顶 299198 次、没图 299198。先查已建，查到就不受上限管；上限只管真新建。
    // Round 96: the cap counted calls, not creations, so re-binding one existing image every frame burned the budget and then
    // even already-built images were refused (299198 hits on the cap). Look the serial up first; the cap only gates new images.
    for (int i = 0; i < g_tex_n; i++) if (g_tex[i].serial == t.serial) return &g_tex[i];
    // 第 109 轮：上限要数「活着的图」（g_tex_n），不能数一生建过的张数（g_tex_make）。饥荒的 UI 一直在 Finalize/新建，
    //   mtl_tex_release 把表从 3000 缩回 21 张，而按一生计数撞顶 475881 次 ⇒ 后半程连一张新图都不给，全算成「没图」。
    // Round 109: cap the LIVE table, not lifetime creations. DST's front end finalizes and rebuilds textures constantly
    //   (mtl_tex_release shrank the table from 3000 back to 21), while the lifetime counter hit the cap 475881 times and
    //   the second half of the run refused every new image - all of them counted as 「没图」.
    if (g_tex_n >= ((g_skip & 131072) ? TEX_MAKE_MAX_HI : TEX_MAKE_MAX)) { atomic_fetch_add(&g_tex_make_full, 1); return NULL; }
    int before = g_tex_n;
    vktex *v = tex_get(&t);
    if (v && g_tex_n != before) atomic_fetch_add(&g_tex_make, 1);
    return v;
}

// 一张图有没有来源：传过像素或当过附件。绑到一张从没写过的图 = 采到全黑，比哑图更难看（第 90 轮的黑屏有这个成分）。
// Whether an image has a source at all: uploaded, or used as an attachment. Binding a never-written image samples pure black,
// which is worse than the dummy - round 90's black frame had a share of that.
static int tex_has_content(const vktex *v) { return v && (v->uploaded || v->drawn); }

// 同一张 NVN 贴图（同 serial）在回放层被建成两页 VkImage 时：游戏往有内容的那页画、着色器采的是空的那页。
// 这一条把「采到空图」当场分成两类——有兄弟（= 拆成了两张，走复用就修好）和没兄弟（= 内容压根没进过 GPU，要找写入点）。
// 判据只算一次并钉在格子上：空图绑定一轮几百万次，不能每次都扫贴图表。
// When one NVN texture (one serial) exists as two VkImages, the game renders into the one with content while the shader reads the
// empty one. This splits every "sampled an empty image" into two kinds - has a sibling (the split is real, reuse fixes it) or has none
// (the bytes never reached the GPU at all). Resolved once per slot: an empty bind happens millions of times, so no table scan each time.
static int tex_sibling_content(vktex *v)
{
    if (!v) return 0;
    if (!v->sibchk) {
        v->sibchk = 1;
        for (int i = 0; i < g_tex_n; i++)
            if (&g_tex[i] != v && g_tex[i].serial == v->serial && tex_has_content(&g_tex[i])) { v->sib = 1; break; }
    }
    return v->sib;
}

// ---- 空图清单（M2-3f 的判读工具，第 111 轮）----
// 第 110 轮实测：打包上传只搬成 8 张，而「空图」哑绑定有 197 万次 —— 说明哑图的大头根本不是打包贴图，
// 而现场行（一次性 GVL）在这条路径上一条都没落盘：hilog 环形缓冲被客体刷屏挤掉了（见 09 文档「开局打的行会被挤掉」那条）。
// 所以改成按 serial 去重攒进表，跟着出图周期行一起吐。每张记：尺寸 / NVN 格式 / 目标 / 层级 / 有没有打包布局 /
// 池内存 8 KB 采样里有多少非零字节 —— 非零 = 像素在客体内存里躺着（该搬），全零 = 内容压根没走内存这条路（该找别的写入点）。
// Round 110 measured it: the packaged path moved 8 images while 1.97M dummy binds had no content, so the dummies are mostly NOT
// packaged textures - and the one-shot scene lines never reached the log at all (the hilog ring buffer rotated under the guest's flood).
// So: dedupe by serial into a table and ride out with the periodic present line, recording size / NVN format / target / levels /
// whether it is packaged / how many non-zero bytes a 8 KiB pool sample holds (non-zero = the pixels sit in guest memory unmoved).
static _Atomic uint64_t g_emp_pk, g_emp_nopk;
#define EMP_MAX 64
typedef struct {
    uint64_t ser;
    uint32_t n, w, h, nz, sn, psz;
    uint16_t fmt;
    uint8_t tgt, lv, la, pk, mem, dep, lsz;
} emp_ent;
static emp_ent g_emp[EMP_MAX];
static int g_emp_n;
static pthread_mutex_t g_empm = PTHREAD_MUTEX_INITIALIZER;

static void emp_note(const vktex *v)
{
    int packaged = v->d.has_layout && v->d.mem;
    atomic_fetch_add(packaged ? &g_emp_pk : &g_emp_nopk, 1);
    pthread_mutex_lock(&g_empm);
    int i;
    for (i = 0; i < g_emp_n; i++) if (g_emp[i].ser == v->serial) break;
    if (i < g_emp_n) g_emp[i].n++;
    else {
        if (g_emp_n >= EMP_MAX) {   // 表满：损失计数——全部减一、掉到 0 的让位。第 118 轮这张 64 格表被开头 64 张占死，
                                    // 之后「哪张空图采得最多」全靠猜；减一让常客自己浮上来。
            int m = 0;
            for (int k = 0; k < g_emp_n; k++) if (g_emp[k].n > 1) { g_emp[k].n--; g_emp[m++] = g_emp[k]; }
            g_emp_n = m;
        }
        if (g_emp_n < EMP_MAX) {
            emp_ent *e = &g_emp[g_emp_n++];
            memset(e, 0, sizeof *e);
            e->ser = v->serial; e->n = 1; e->w = v->w; e->h = v->h;
            e->fmt = (uint16_t)v->d.d.format; e->tgt = (uint8_t)v->d.d.target;
            e->lv = (uint8_t)v->levels; e->la = (uint8_t)v->layers;
            e->pk = (uint8_t)(v->d.has_layout != 0); e->mem = (uint8_t)(v->d.mem != NULL);
            e->dep = (uint8_t)v->is_depth;
            e->lsz = (uint8_t)(v->d.L.size >> 10 > 255 ? 255 : v->d.L.size >> 10);
            if (v->d.mem && v->d.pool) {   // 采样点与 mtl_read_texels 对齐：指针就是 mem（它已经指到池内这张图的开头）
                uint64_t av = v->d.pool->size > (uint64_t)v->d.off ? v->d.pool->size - (uint64_t)v->d.off : 0;
                size_t sn = av < 8192 ? (size_t)av : 8192;
                const uint8_t *q = v->d.mem;
                e->psz = (uint32_t)(v->d.pool->size >> 20);
                e->sn = (uint32_t)sn;
                for (size_t b = 0; b < sn; b++) if (q[b]) e->nz++;
            }
        }
    }
    pthread_mutex_unlock(&g_empm);
}

static int emp_line(char *out, int cap)
{
    static emp_ent tmp[EMP_MAX];
    pthread_mutex_lock(&g_empm);
    int n = g_emp_n;
    memcpy(tmp, g_emp, (size_t)n * sizeof *tmp);
    pthread_mutex_unlock(&g_empm);
    int m = snprintf(out, (size_t)cap, "[vk] EMP pk=%llu nopk=%llu %d/%d",
                     (unsigned long long)atomic_load(&g_emp_pk), (unsigned long long)atomic_load(&g_emp_nopk), n, EMP_MAX);
    for (int k = 0; k < 6 && k < n; k++) {   // 选择排序取前 6：绑定次数最多的那几张才是画面缺的内容
        int best = k;
        for (int i = k + 1; i < n; i++) if (tmp[i].n > tmp[best].n) best = i;
        if (best != k) { emp_ent s = tmp[k]; tmp[k] = tmp[best]; tmp[best] = s; }
        if (m > cap - 96) break;
        m += snprintf(out + m, (size_t)(cap - m), " |s%llu %ux%u f%d t%d L%d la%d pk%d m%d d%d K%d P%u nz%u/%u n%u",
                      (unsigned long long)tmp[k].ser, tmp[k].w, tmp[k].h, (int)tmp[k].fmt, (int)tmp[k].tgt,
                      (int)tmp[k].lv, (int)tmp[k].la, (int)tmp[k].pk, (int)tmp[k].mem, (int)tmp[k].dep,
                      (int)tmp[k].lsz, tmp[k].psz, tmp[k].nz, tmp[k].sn, tmp[k].n);
    }
    return m;
}

// 释放（游戏 Finalize 贴图时）：从表里摘掉并销毁 | release (the game finalized the texture): drop it from the table and destroy it
void mtl_tex_release(tex_t *t)
{
    if (!g_on || !t) return;
    // 描述符集里存的正是这张视图：图一销毁，缓存的那几条必须作废，否则下一帧绑的是已销毁的 view
    // A cached descriptor set points at this view: dropping the texture invalidates those entries, or the next frame binds a destroyed view.
    for (int i = 0; i < TRISET_N; i++) if (g_tri_set[i].serial == t->serial) {
        if (g_tri_pool && g_tri_set[i].set) vkFreeDescriptorSets(g_dev, g_tri_pool, 1, &g_tri_set[i].set);
        g_tri_set[i].set = NULL; g_tri_set[i].serial = 0;
    }
    for (int i = 0; i < g_tex_n; i++) {
        if (g_tex[i].serial != t->serial) continue;
        vktex *v = &g_tex[i];
        // 进回收站，不当场销毁：饥荒前端每帧 Finalize+Initialize 同一块池内存，留着这块显存和里面的像素，
        // 下一帧同身份重建直接复活成有内容的图，不会再出现「本帧新建、本帧就采样」的空图。
        // Park it in the bin instead of destroying: DST finalizes and re-initializes the same pool storage every frame, so keeping the
        // allocation and its pixels lets the next life with the same identity come back already filled, not empty-on-its-first-frame.
        if (!(g_skip & 65536) && recy_put(v)) { /* 句柄归回收站 */ }
        else tex_destroy(v);
        g_tex_n--;
        if (i != g_tex_n) g_tex[i] = g_tex[g_tex_n];
        memset(&g_tex[g_tex_n], 0, sizeof g_tex[g_tex_n]);
        g_tex_gen++;
        g_sets_valid = 0;   // 缓存的描述符集里可能还指着这张视图 | a cached set may still point at this view
        return;
    }
}

// 布局转换：只在需要时发屏障（本刀只有 UNDEFINED→ATTACH/TRANSFER 几条路）
// Layout transition: issue a barrier only when needed (this cut only walks UNDEFINED -> attachment / transfer)
static void to_layout(VkCommandBuffer cb, vktex *v, VkImageLayout want)
{
    if (v->layout == want) return;
    // 阶段掩码与访问掩码是两套位，必须分开算：0x20000 是 COLOR_ATTACHMENT_WRITE（访问位），
    // 当作阶段位传进 vkCmdPipelineBarrier 就是未定义的阶段——Maleoon 收下后按错误的阶段排依赖，
    // 依赖没盖住的那次写就是 GPU 故障（本层没有校验层，这类错只能靠读码抓到）。
    // Stage masks and access masks are two different bit sets. 0x20000 is VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; passing it as a stage bit is
    // an undefined stage, so the driver orders the dependency wrong and the write it failed to cover faults the GPU. No validation layer
    // here, so this class of bug only comes out of reading the code.
    VkPipelineStageFlags src_stage, dst_stage;
    VkAccessFlags src_acc = 0, dst_acc = 0;
    switch (v->layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                                           src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                                           src_acc   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
                                           src_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                                           src_acc   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                                           src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; src_acc = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                                           src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; src_acc = VK_ACCESS_SHADER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
                                           src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
                                           src_acc   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT; break;
    default:                              src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; src_acc = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; break;
    }
    switch (want) {
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                                           dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; dst_acc = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                                           dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                                           dst_acc   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
                                           dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                                           dst_acc   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                                           dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; dst_acc = VK_ACCESS_SHADER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
                                           dst_acc   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT; break;
    default:                              dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; dst_acc = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; break;
    }
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = v->layout;
    b.newLayout = want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = v->img;
    b.subresourceRange = (VkImageSubresourceRange){ v->is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, v->levels, 0, v->layers };
    b.srcAccessMask = src_acc;
    b.dstAccessMask = dst_acc;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    v->layout = want;
}

// ---------------- 通道与绘制状态 | passes and draw state ----------------
typedef struct { VkRenderingAttachmentInfo col[8]; uint32_t ncol; VkRenderingAttachmentInfo ds; uint32_t has_ds; vktex *cv[8]; vktex *dv;
                 VkRenderPass rp; uint64_t pk; } curpass;   // rp/pk：本通道实际用的经典渲染通道与其键，管线照它建（M2-3b）| the pass this framebuffer was built for; pipelines are built against it
static curpass g_p;

static void att(VkRenderingAttachmentInfo *a, VkImageView view, VkImageLayout lay, const float *clear, int clear_it)
{
    memset(a, 0, sizeof *a);
    a->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a->imageView = view;
    a->imageLayout = lay;
    a->loadOp = clear_it ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear_it && clear) { a->clearValue.color.float32[0] = clear[0]; a->clearValue.color.float32[1] = clear[1]; a->clearValue.color.float32[2] = clear[2]; a->clearValue.color.float32[3] = clear[3]; }
}

static void pkg_flush(void);   // 定义在拷贝那一节：通道外把排队中的打包贴图搬进显卡 | defined with the copy ops: push queued packaged textures outside a pass
static void pkg_queue(vktex *v);   // 绑定看到「有图没来源」时排一张，等通道外冲刷 | queue an image that has no source yet; flushed outside the pass

static void pass_end(void)
{
    if (!g_in_pass || !g_cb) return;
    vkCmdEndRenderPass(g_cb);
    g_in_pass = 0;
    // 通道一关就把颜色附件转成可采样：游戏画完一张图下一步就是把它当贴图采（后处理、界面合成全是这个形状）。
    // 只能在通道外转：通道内对这些图发布局屏障，Vulkan 对通道内可用的阶段有额外限制，而本层没有校验层可试。
    // Close the pass, then move every colour attachment to sampleable: the game draws into a texture and samples it right after
    // (postprocess chains and the UI composite are exactly this shape). It has to happen outside the pass - barriers inside a pass
    // carry extra stage-mask restrictions and there is no validation layer here to lean on.
    for (int i = 0; i < g_p.ncol; i++) if (g_p.cv[i]) to_layout(g_cb, g_p.cv[i], col_rest());
    pkg_flush();   // 通道外才发得了 transfer：把这一趟绑定时排下的打包贴图搬进显卡 | transfers only work outside a pass - move the queued packaged textures now
}

// 经典渲染通道缓存：键 = 附件格式的组合。本层从动态渲染（vkCmdBeginRendering）改成经典通道，
// 是因为实测把「游戏的动态通道」和「出图的经典通道」同时开在同一队列上，几十到几百帧后必出 GPU 故障
// （第 49/51/57 轮：present = -4 或游戏提交 = -4；而任一条单独跑——第 56 轮 SKIP=1、SKIP=256——都是几百帧全绿）。
// 全流程只留一种写法之后，Maleoon 少一个没被证实能正确处理的组合，而管线那一刀本来也要按 VkRenderPass 建。
// Classic render pass cache, keyed by the attachment format set. The layer switched from dynamic rendering (vkCmdBeginRendering) to
// classic passes because running the game's dynamic passes and the present's classic pass on the same queue always ended in a GPU fault
// after some dozens-to-hundreds of frames (rounds 49/51/57: present = -4 or a failed game submit), while either stream alone ran green
// for hundreds of frames (round 56, SKIP=1 and SKIP=256). With one style in the whole process the driver has one combination left it has
// not been shown to mishandle - and the pipeline cut needs VkRenderPass objects anyway.
typedef struct { uint64_t key; VkRenderPass rp; } rpcache;
typedef struct { uint64_t key; VkRenderPass rp; VkImageView v[9]; uint32_t ncol; int ds; uint32_t w, h; VkFramebuffer fb; } fbcache;
#define RPC_N 64
#define FBC_N 256
static rpcache g_rpc[RPC_N]; static int g_rpc_n;
static fbcache g_fbc[FBC_N]; static int g_fbc_n;

static uint64_t pass_key(void)
{
    uint64_t k = g_p.ncol * 1315423911u;
    for (uint32_t i = 0; i < g_p.ncol; i++) k = k * 1315423911u + (uint64_t)g_p.cv[i]->fmt * 7u + (uint64_t)g_p.cv[i]->is_depth;
    if (g_p.has_ds) k = k * 1315423911u + (uint64_t)g_p.dv->fmt * 3u + 1u;
    return k;
}

// 通道外附件停在哪一态：第 90 轮把颜色停在 SHADER_READ_ONLY、深度停在 DEPTH_READ_ONLY（着色器合法可读的那一态），
// 但同批还上了别的改动，画面从白回落到逐位纯黑，所以留两个位把这两处单独退回旧写法做 A/B：
// 位 4096 = 颜色回 COLOR_ATTACHMENT，位 8192 = 深度回 DEPTH_STENCIL_ATTACHMENT（并顺带不绑真深度视图）。
// Which layout attachments rest in outside a pass. Round 90 parked colour in SHADER_READ_ONLY and depth in DEPTH_READ_ONLY
// (the layout a shader may legally sample from), but that shipped in the same batch as other changes and the frame went from
// white back to a bit-identical black, so two bits rewind each half on its own for an A/B.
static VkImageLayout col_rest(void)
{
    return (g_skip & 4096) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
static VkImageLayout dep_rest(void)
{
    return (g_skip & 8192) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
}

static VkRenderPass rp_get(uint64_t key)
{
    for (int i = 0; i < g_rpc_n; i++) if (g_rpc[i].key == key) return g_rpc[i].rp;
    if (g_rpc_n >= RPC_N) { GRED("通道缓存满了（%{public}d），这张通道不开", RPC_N); return NULL; }
    VkAttachmentDescription ad[9]; VkAttachmentReference ar[8];
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_p.ncol; i++) {
        // loadOp 用 LOAD 不用 DONT_CARE（第 84 轮改）：原来每次开通道都把这张图抹掉，而游戏是一张目标反复开合好几趟
        // 用的，抹完就只剩它自己那次清除值 ⇒ 出图源永远是黑的。游戏的清除走 OP_CLEAR_COLOR → vkCmdClearAttachments，
        // 本来就不靠通道的 loadOp。
        // loadOp is LOAD, not DONT_CARE (changed in round 84): discarding on every pass open left only the game's own clear value,
        // so the present source stayed black. The game clears through vkCmdClearAttachments and never relied on loadOp.
        // 通道外的态改成 SHADER_READ_ONLY：游戏把一张目标反复开合好几趟，后一趟要用着色器读回上一趟写进去的内容
        // （后处理、UI 合成都是这个写法）。停在 COLOR_ATTACHMENT 而描述符声明 SHADER_READ_ONLY 是两套态，
        // 块同步 GPU 上读到的就是没落盘的图块——第 88 轮「布局未就」记的正是这件事。
        // Outside a pass the colour attachments now rest in SHADER_READ_ONLY. The game re-opens one target several times and the
        // later pass reads back what the earlier one wrote (post-processing, UI composition). Leaving it in COLOR_ATTACHMENT while
        // the descriptor declares SHADER_READ_ONLY means two different layouts, and on a tiled GPU that read gets unresolved tiles -
        // exactly what round 88 counted as 「布局未就」.
        ad[n] = (VkAttachmentDescription){ 0, g_p.cv[i]->fmt, VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
            col_rest(), col_rest() };
        ar[i] = (VkAttachmentReference){ n, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        n++;
    }
    VkAttachmentReference dr = { n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    if (g_p.has_ds) {
        // 深度也要 STORE：游戏常见写法是「先开一趟只挂深度的通道做深度预通道（或阴影图），
        // 再开挂颜色的通道带深度测试画」。这里 DONT_CARE 等于把上一趟写进深度的结果抹掉，
        // 而本趟的 loadOp 是 LOAD ⇒ 读进来的是未定义深度，测试结果就成了随机丢弃。
        // Depth must STORE too: the game opens a depth-only pass first (pre-pass / shadow map) and then a colour pass with a depth test.
        // DONT_CARE wiped what that first pass wrote while this one loads it, so the test compared against undefined depth.
        // 通道内用 ATTACHMENT、通道外停在 DEPTH_READ_ONLY：附件引用（dr）声明的是通道里那一态，初/终态是通道外的态。
        // 这样一趟结束后深度图就停在可被着色器合法读的那一态，下一趟阴影采样不必再补屏障。
        // In-pass layout stays ATTACHMENT (that is what the subpass reference declares) while initial/final become
        // DEPTH_READ_ONLY, so after a pass the depth image rests in the one layout a shader may legally sample from -
        // the next pass' shadow read then needs no extra barrier.
        ad[n] = (VkAttachmentDescription){ 0, g_p.dv->fmt, VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
            dep_rest(), dep_rest() };
        n++;
    }
    VkSubpassDescription sp = {0};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = g_p.ncol;
    sp.pColorAttachments = ar;
    if (g_p.has_ds) sp.pDepthStencilAttachment = &dr;
    // 外部依赖：上一次用这张图（着色/搬运）到本次通道结束的读写，按块同步
    // External dependency: previous use of these images up to this pass' reads/writes, by-region
    // 字段序是 srcSubpass, dstSubpass, **srcStageMask, dstStageMask**, srcAccessMask, dstAccessMask, flags
    // —— 原来把访问位填进了阶段位那两格（坑 33 的反面），依赖等于没排；现在改成 LOAD 语义，
    // 通道要读上一趟留在图里的内容，这条排序必须真生效，否则读到的就是竞态垃圾。
    // The struct's 3rd/4th slots are STAGE masks, not access masks - the old literal put access bits there, so the
    // dependency ordered nothing. With the pass now loading previous content, that ordering is load-bearing.
    VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT | (g_p.has_ds ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0),
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | (g_p.has_ds ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0),
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT | (g_p.has_ds ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : 0),
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        (g_p.has_ds ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : 0),
        VK_DEPENDENCY_BY_REGION_BIT };
    VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpc.attachmentCount = n; rpc.pAttachments = ad;
    rpc.subpassCount = 1; rpc.pSubpasses = &sp;
    rpc.dependencyCount = 1; rpc.pDependencies = &dep;
    VkRenderPass rp = NULL;
    if (vkCreateRenderPass(g_dev, &rpc, NULL, &rp) != VK_SUCCESS) { GRED("vkCreateRenderPass(游戏) 失败"); return NULL; }
    g_rpc[g_rpc_n++] = (rpcache){ key, rp };
    if (g_rpc_n <= 8) GVL("[vk] 新游戏通道 %{public}d（色 %{public}u 深 %{public}d，累计 %{public}d 条）", (int)g_rpc_n - 1, g_p.ncol, g_p.has_ds, g_rpc_n);
    return rp;
}

static VkFramebuffer fb_get(uint64_t key, VkRenderPass rp, VkExtent2D ext, int has_ds)
{
    uint32_t ncol = g_p.ncol;
    for (int i = 0; i < g_fbc_n; i++)
        if (g_fbc[i].key == key && g_fbc[i].w == ext.width && g_fbc[i].h == ext.height && g_fbc[i].ncol == ncol && g_fbc[i].ds == has_ds) {
            int same = 1;
            for (uint32_t c = 0; c < ncol; c++) if (g_fbc[i].v[c] != g_p.cv[c]->view) same = 0;
            if (has_ds && g_fbc[i].v[ncol] != g_p.dv->view) same = 0;
            if (same) return g_fbc[i].fb;
        }
    if (g_fbc_n >= FBC_N) { GRED("帧缓冲缓存满了（%{public}d）", FBC_N); return NULL; }
    VkImageView av[9]; uint32_t n = 0;
    for (uint32_t i = 0; i < ncol; i++) av[n++] = g_p.cv[i]->view;
    if (has_ds) av[n++] = g_p.dv->view;
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = rp;
    fbi.attachmentCount = n; fbi.pAttachments = av;
    fbi.width = ext.width; fbi.height = ext.height; fbi.layers = 1;
    VkFramebuffer fb = NULL;
    if (vkCreateFramebuffer(g_dev, &fbi, NULL, &fb) != VK_SUCCESS) { GRED("vkCreateFramebuffer(游戏) 失败 %{public}u x %{public}u", ext.width, ext.height); return NULL; }
    g_fbc[g_fbc_n] = (fbcache){ key, rp, {0}, ncol, has_ds, ext.width, ext.height, fb };
    for (uint32_t i = 0; i < n && i < 9; i++) g_fbc[g_fbc_n].v[i] = av[i];
    g_fbc_n++;
    return fb;
}

// 通道可画范围 = 所有附件的最小尺寸。NVN 允许一张通道里挂不同大小的贴图，而 Vulkan 的 renderArea
// 超出版本会被驱动直接拒（第 37 轮 RENDERPASS："Wrong extent width of renderarea … framebuffer(800,450)"）。
// 所以这里不看交换链尺寸——游戏通道和屏幕尺寸本来就是两回事。
// The drawable extent of a pass = the minimum over its attachments. NVN lets one pass carry textures of different sizes, but a renderArea
// beyond the framebuffer is rejected by the driver (round 37 RENDERPASS: "Wrong extent width of renderarea ... framebuffer(800,450)").
// The swapchain extent is deliberately not consulted - a game pass and the screen are two different sizes.
static VkExtent2D att_min(void)
{
    VkExtent2D ar = { 0xFFFFFFFFu, 0xFFFFFFFFu };
    for (uint32_t i = 0; i < g_p.ncol; i++) { if (g_p.cv[i]->w < ar.width) ar.width = g_p.cv[i]->w; if (g_p.cv[i]->h < ar.height) ar.height = g_p.cv[i]->h; }
    if (g_p.has_ds) { if (g_p.dv->w < ar.width) ar.width = g_p.dv->w; if (g_p.dv->h < ar.height) ar.height = g_p.dv->h; }
    if (ar.width == 0xFFFFFFFFu) ar = (VkExtent2D){ 1, 1 };
    return ar;
}

// OP_SET_RT：换通道。本刀只挂颜色附件（深度后续刀补），清由 OP_CLEAR_* 发
// OP_SET_RT: switch passes. This cut attaches color only (depth comes later); clears come from OP_CLEAR_*
static void do_set_rt(const p_set_rt *r)
{
    pass_end();
    memset(&g_p, 0, sizeof g_p);
    g_att_src = 0;
    g_in1_cur = 0;
    // 只读前 r->n 个：SetRenderTargets 只写它声明的那几个槽，剩下的在记录里是未初始化内存。
    // 按 8 个槽读会把垃圾指针当贴图解引用（第 38 轮 mtl_exec+0x73c 的 SEGV@0x800000005e 就是这么来的）。
    // Only the first r->n slots are meaningful: SetRenderTargets writes just the ones it declares, the rest of the record is uninitialized.
    // Reading all eight dereferences garbage as a texture pointer (round 38: SEGV@0x800000005e at mtl_exec+0x73c).
    int ncol_req = r->n < 0 ? 0 : (r->n > 8 ? 8 : r->n);
    VkImageView used[9]; int nused = 0;   // 同一个视图在一个通道里只能挂一次（重复附件 / 颜色与深度同图在 Vulkan 里是非法用法，Maleoon 直接判 DEVICE_LOST）
                                          // One view may be attached only once per pass: duplicate attachments, or colour and depth on the same image, are illegal usage and Maleoon answers with DEVICE_LOST
    for (int i = 0; i < ncol_req; i++) {
        if (!r->color[i]) continue;
        vktex *v = tex_get(r->color[i]);
        if (!v || !v->view) continue;
        int dup = 0;
        for (int k = 0; k < nused; k++) if (used[k] == v->view) { dup = 1; break; }
        if (dup) continue;
        if (nused < 9) used[nused++] = v->view;
        for (int k = 0; k < g_src_n; k++) if (g_src_v[k] == v) { atomic_fetch_add(&g_rt_src_pass, 1); atomic_fetch_add(&g_src_att[k], 1); g_att_src = 1; break; }
        g_p.cv[g_p.ncol] = v;
        v->drawn = 1;                       // 当过附件 = 这张图会有内容（按需建图那条路的判据之一）
        if (g_sp_tex[0] && v == g_sp_tex[0]) { g_in1_cur = 1; atomic_fetch_add(&g_in1_pass, 1); }
        att(&g_p.col[g_p.ncol], v->view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, NULL, 0);
        g_p.ncol++;
    }
    if (r->depth) {
        vktex *v = tex_get(r->depth);
        if (v && v->view) {
            int dup = 0;
            for (int k = 0; k < nused; k++) if (used[k] == v->view) dup = 1;
            if (dup) { v = NULL; GRED("深度附件与颜色同图：这刀不挂深度"); }
        }
        if (v && v->view && !(g_skip & 4)) {
            g_p.dv = v;
            v->drawn = 1;                   // 同上：深度附件写过就有内容
            VkClearValue c = { .depthStencil = { 1.0f, 0 } };
            memset(&g_p.ds, 0, sizeof g_p.ds);
            g_p.ds.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            g_p.ds.imageView = v->view;
            g_p.ds.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            g_p.ds.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            g_p.ds.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            g_p.ds.clearValue = c;
            g_p.has_ds = 1;
        }
    }
    if (!g_p.ncol && !g_p.has_ds) { g_rt_noatt++; return; }
    if (!g_cb) { g_rt_nocb++; return; }
    VkExtent2D ext = att_min();
    uint64_t key = pass_key();
    VkRenderPass rp = rp_get(key);
    if (!rp) { g_rt_rp++; return; }
    VkFramebuffer fb = fb_get(key, rp, ext, g_p.has_ds ? 1 : 0);
    if (!fb) { g_rt_fb++; return; }
    // 通道改成 LOAD 语义之后，附件必须**已经在**通道的初态里：通道自己不做入口转换（初态就是 READ_ONLY 一族），
    // 所以这里在 BeginRenderPass **之前**按 vktex 记账的当前布局补一条屏障（游戏侧只有这一处会用到非附件布局）。
    // With the pass loading instead of discarding, attachments must already sit in the pass' initial layout - the pass does no
    // transition on entry, so the barrier goes here, before BeginRenderPass, driven by the layout tracked on the vktex.
    for (uint32_t i = 0; i < g_p.ncol; i++) to_layout(g_cb, g_p.cv[i], col_rest());
    if (g_p.has_ds) to_layout(g_cb, g_p.dv, dep_rest());   // 通道的初态就是这一态（见 rp_get），通道自己转进 ATTACHMENT
    VkClearValue cv[9];
    VkRenderPassBeginInfo rpb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpb.renderPass = rp;
    rpb.framebuffer = fb;
    rpb.renderArea = (VkRect2D){ { 0, 0 }, ext };
    rpb.clearValueCount = 0;
    memset(cv, 0, sizeof cv);
    vkCmdBeginRenderPass(g_cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    g_p.rp = rp; g_p.pk = key;      // 管线要照这条通道建 | pipelines are built against this pass
    g_in_pass = 1;
    atomic_fetch_add(&g_passes, 1);
    for (int i = 0; i < 8; i++) {   // 尺寸直方图：只认前 8 种，够看出「游戏根本没往屏幕尺寸那张画」
        if (g_rtsize[i].w == ext.width && g_rtsize[i].h == ext.height) { atomic_fetch_add(&g_rtsize[i].n, 1); break; }
        if (!g_rtsize[i].w) { g_rtsize[i].w = ext.width; g_rtsize[i].h = ext.height; atomic_fetch_add(&g_rtsize[i].n, 1); break; }
    }
}

static void do_clear_color(const p_clear_color *r)
{
    if (!g_in_pass || !g_cb) return;
    VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, {{0}} };
    ca.colorAttachment = (uint32_t)(r->index < 0 ? 0 : r->index);
    if (ca.colorAttachment >= g_p.ncol) return;   // 清了没挂的槽：驱动不会替我们圆 | clearing an unattached slot: the driver will not paper over it for us
    ca.clearValue.color.float32[0] = r->rgba[0]; ca.clearValue.color.float32[1] = r->rgba[1];
    ca.clearValue.color.float32[2] = r->rgba[2]; ca.clearValue.color.float32[3] = r->rgba[3];
    VkClearRect cr = { .rect = (VkRect2D){ { 0, 0 }, att_min() }, .baseArrayLayer = 0, .layerCount = 1 };
    vkCmdClearAttachments(g_cb, 1, &ca, 1, &cr);
    atomic_fetch_add(&g_clears, 1);
}

static void do_clear_ds(const p_clear_ds *r)
{
    if (!g_in_pass || !g_cb || !g_p.has_ds) return;
    VkClearAttachment ca = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, {{0}} };
    ca.clearValue.depthStencil.depth = r->depth;
    VkClearRect cr = { .rect = (VkRect2D){ { 0, 0 }, att_min() }, .baseArrayLayer = 0, .layerCount = 1 };
    vkCmdClearAttachments(g_cb, 1, &ca, 1, &cr);
}

// ---------------- 提交 | submission ----------------
// 驱动报错时把这条命令缓冲的构成打出来（op 直方图）：没有校验层，这是唯一能定位「哪类命令让 Maleoon 翻脸」的手段
// On a driver fault print this buffer's composition (the op histogram). With no validation layer on device, this is the only way to pin down
// which class of command makes Maleoon angry.
static void buf_ops_dump(const char *why)
{
    static int once;
    if (once >= 4) return;
    once++;
    char line[900]; int n = 0;
    for (int i = 0; i < OP_COUNT_ && n < (int)sizeof line - 24; i++)
        if (g_buf_ops[i]) n += snprintf(line + n, sizeof line - n, " op%d:%llu", i, (unsigned long long)g_buf_ops[i]);
    GVL("[vk] 失败现场(%{public}s)：通道 %{public}u 清 %{public}u 提交 %{public}u%{public}s", why,
        (unsigned)atomic_load(&g_passes), (unsigned)atomic_load(&g_clears), (unsigned)g_submits, line);
}

static void cb_begin(void)
{
    if (g_cb) return;
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = g_cp;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_dev, &ca, &g_cb) != VK_SUCCESS) { g_cb = NULL; GRED("分配命令缓冲失败"); return; }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cb, &bi);
}

void mtl_sub_begin(void)
{
    if (!vk_ensure()) return;
    memset(g_buf_ops, 0, sizeof g_buf_ops);
    cb_begin();
    mtl_sub_reset_draw();   // 一次提交 = arena 换一代、描述符池清空 | one submission = a fresh arena generation and an emptied descriptor pool
}

void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }

uint64_t mtl_sub_commit(void)
{
    if (!vk_ensure()) return ++g_fence_val;
    // 谁在提交：命令池 g_cp 不是线程安全对象，出图和游戏若落在两个线程上，同池分配就是竞争
    // Who submits: the command pool g_cp is not thread-safe, so allocating from it on two threads (game + present) is a race.
    int tid = (int)syscall(SYS_gettid);
    if (!g_sub_tid) g_sub_tid = tid;
    else if (g_sub_tid != tid) { g_sub_tid = -1; }
    pass_end();
    if (g_skip & 256) { if (!g_cb) return ++g_fence_val;   // 位 256 = 游戏这条提交整个不上 GPU：连空缓冲都不飞（出图单独背锅时用）
        vkEndCommandBuffer(g_cb); vkFreeCommandBuffers(g_dev, g_cp, 1, &g_cb); g_cb = NULL; return ++g_fence_val; }
    if (!g_in_pass && g_cb) pkg_flush();   // 没有通道收尾时也要冲刷：这批图不搬就永远停在哑图 | flush even when no pass closed - otherwise these images stay dummies forever
    if (!g_cb) return ++g_fence_val;
    if (vkEndCommandBuffer(g_cb) != VK_SUCCESS) { GRED("vkEndCommandBuffer 失败"); g_cb = NULL; return ++g_fence_val; }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    VkResult cr = vkQueueSubmit(g_q, 1, &si, g_fence);
    if (cr != VK_SUCCESS) { GRED("游戏提交 = %{public}d (%{public}s)｜tid %{public}d", (int)cr, vr(cr), g_sub_tid); buf_ops_dump("游戏提交"); }
    g_submits++;
    // 先等栅栏、再 reset、最后才回收命令缓冲。两条都是硬规定：
    //   · 缓冲还在飞就 vkFreeCommandBuffers（VUID-vkFreeCommandBuffers-pCommandBuffers-00065）会把池里正在用的内存放回空闲链，
    //     下一次分配拿去重用——出图和游戏各自单跑时看不出来，两条流一起跑就踩雷（第 49/51/57/58/59 轮的 present = -4 全在这上面）；
    //   · 栅栏还在 signaled 状态就再次提交（VUID-vkQueueSubmit-fence-00064）：本函数的 g_fence 从建好到现在一次都没 reset 过。
    // Wait the fence, reset it, and only then recycle the command buffer. Both are hard rules: freeing a buffer that is still executing
    // hands memory the driver is using back to the pool's free list, and the next allocation reuses it - invisible while only one stream
    // runs, fatal once the game and the present both do (that is rounds 49/51/57/58/59's present = -4). And g_fence had never been reset,
    // so every submit after the first reused a signaled fence.
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g_dev, 1, &g_fence);
    vkFreeCommandBuffers(g_dev, g_cp, 1, &g_cb);
    g_cb = NULL;
    g_fence_val++;
    return g_fence_val;
}

uint64_t mtl_last_fence(void) { return g_fence_val; }

// ---------------- 出图源读回 | reading the present source back to the CPU ----------------
// 屏幕一直是同一张黑（第 82 轮的截图与黑屏基线逐字节相同），但「游戏那张图本来就是黑」和
// 「图上有内容、只是被 alpha 合成吃掉 / 采样接错」这两种情况在截图上分不开。
// 这一把把它们分开：每 300 帧把出图源整张拷进一块 host-visible 缓冲，等出图栅栏落地后统计。
// The screen has been the same black since round 82 (byte-identical to the black baseline), but "the game's target really is
// black" and "it has content that alpha compositing / the sampling wiring hides" are indistinguishable in a screenshot.
// This splits them: every 300 frames copy the whole source into a host-visible buffer and summarize it after the fence.
static int g_rb_on;                                       // DM_VK_RB=1 才做（默认关：一次 5.7 MB）
static VkBuffer g_rb_buf; static VkDeviceMemory g_rb_mem; static uint8_t *g_rb_map;
static uint64_t g_rb_cap; static uint32_t g_rb_w, g_rb_h; static uint64_t g_rb_serial;
static int g_rb_skip_said;

static int mk_hostbuf(uint64_t size, uint32_t usage, VkBuffer *b, VkDeviceMemory *m, void **map);   // 定义在下面 | defined below

static int rb_arm(VkCommandBuffer cb, vktex *v)
{
    if (v->is_depth || v->fmt != VK_FORMAT_R8G8B8A8_UNORM || v->layers != 1 || v->levels != 1) {
        if (!g_rb_skip_said++) GVL("[vk] RB skip: not RGBA8 single-level fmt=%{public}d layers=%{public}u levels=%{public}u",
                                   (int)v->fmt, v->layers, v->levels);
        return 0;
    }
    uint64_t need = (uint64_t)v->w * v->h * 4u;
    if (need > 96u * 1024u * 1024u) return 0;
    if (need > g_rb_cap) {
        if (g_rb_buf) { vkDestroyBuffer(g_dev, g_rb_buf, NULL); vkFreeMemory(g_dev, g_rb_mem, NULL); g_rb_buf = NULL; g_rb_map = NULL; g_rb_cap = 0; }
        if (!mk_hostbuf(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_rb_buf, &g_rb_mem, (void **)&g_rb_map)) {
            GRED("RB 建读回缓冲失败 %{public}llu", (unsigned long long)need);
            g_rb_on = 0; return 0;
        }
        g_rb_cap = need;
        GVL("[vk] RB 读回缓冲 %{public}llu 字节", (unsigned long long)need);
    }
    to_layout(cb, v, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rc.imageExtent = (VkExtent3D){ v->w, v->h, 1 };
    vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb_buf, 1, &rc);
    g_rb_w = v->w; g_rb_h = v->h; g_rb_serial = v->serial;
    return 1;
}

// 出图栅栏之后调用：这时拷贝一定执行完了（同一条队列，按提交序）
// Called after the present fence: the copy has run, since it went through the same queue ahead of the fence.
static void rb_ascii(void)
{
    if (!g_rb_map || !g_rb_w) return;
    uint32_t w = g_rb_w, h = g_rb_h;
    const int CX = 48, ROWS = 16;
    int bh = (int)h / ROWS, bw = (int)w / CX;
    if (bh < 1 || bw < 1) return;
    static const char ramp[] = " .:-=+*#%@";
    char line[128];
    snprintf(line, sizeof line, "[vk] RB图 s=%llu %ux%u", (unsigned long long)g_rb_serial, w, h);
    GVL("%{public}s", line);
    for (int by = 0; by < ROWS; by++) {
        int n = snprintf(line, sizeof line, "[vk]  |");
        for (int bx = 0; bx < CX; bx++) {
            uint64_t s = 0; uint32_t cnt = 0, lo[3] = { 255u, 255u, 255u }, hi[3] = { 0, 0, 0 };
            for (int y = by * bh; y < (by + 1) * bh; y += 3)
                for (int x = bx * bw; x < (bx + 1) * bw; x += 3) {
                    const uint8_t *q = g_rb_map + ((uint64_t)y * w + x) * 4u;
                    for (int c = 0; c < 3; c++) { s += q[c]; if (q[c] < lo[c]) lo[c] = q[c]; if (q[c] > hi[c]) hi[c] = q[c]; }
                    cnt++;
                }
            unsigned L = cnt ? (unsigned)(s / (cnt * 3u)) : 0;      // 块内三通道平均亮度 0..255 | block mean over R,G,B
            unsigned sat = (unsigned)((hi[0] > lo[0] ? hi[0] - lo[0] : lo[0] - hi[0])
                                    + (hi[1] > lo[1] ? hi[1] - lo[1] : lo[1] - hi[1])
                                    + (hi[2] > lo[2] ? hi[2] - lo[2] : lo[2] - hi[2]));
            unsigned idx = L * 9u / 255u; if (idx > 9u) idx = 9u;
            line[n++] = (char)((sat > 48u) ? ('A' + (L * 25u / 255u > 25u ? 25u : L * 25u / 255u)) : ramp[idx]);
        }
        line[n] = 0;
        GVL("%{public}s", line);
    }
}

// 把读回的那张图原样落一张 TGA（无压缩、BGR(A)、左上原点），hdc file recv 回来就是像素级现场。
// 第 111 轮之后这是主要的看图手段：ASCII 阶梯分不出「几何错位」和「颜色错」，一张图能。
// Dump the readback as an uncompressed top-left TGA so a round can be judged on real pixels instead of a 48x16 ASCII ramp.
static void rb_tga(void)
{
    const char *on = getenv("DM_RBIMG");
    if (!on || on[0] == '0') return;
    const char *d = getenv("DM_TEMP");
    if (!d || !*d) d = getenv("DM_SAVE");
    if (!d || !*d) return;
    uint32_t w = g_rb_w, h = g_rb_h;
    if (!w || w > 0xFFFFu || h > 0xFFFFu) return;
    char path[512];
    // 每个 serial 只留前 3 张：出图早期那几张才有内容（第 116 轮实测上屏那张从 ~540 帧起 checksum 就不再变，
    // 只存最后一张会把有现场的帧全冲掉）
    // Keep only the first 3 dumps per serial: the early frames are the ones with content (round 116 measured the present
    // target's checksum frozen from ~frame 540, so overwriting one slot would erase every frame that had anything to show).
    static int tga_n[8]; static unsigned tga_ser[8]; static int tga_cnt;
    int slot = -1;
    for (int i = 0; i < tga_cnt; i++) if (tga_ser[i] == g_rb_serial) { slot = i; break; }
    if (slot < 0 && tga_cnt < 8) { slot = tga_cnt; tga_ser[slot] = g_rb_serial; tga_n[slot] = 0; tga_cnt++; }
    if (slot < 0 || ++tga_n[slot] > 3) return;
    snprintf(path, sizeof path, "%s/rb_s%llu_%d.tga", d, (unsigned long long)g_rb_serial, tga_n[slot]);
    FILE *f = fopen(path, "wb");
    if (!f) { GVL("[vk] RB图落盘失败 %{public}s", path); return; }
    uint8_t hdr[18] = { 0 };
    hdr[2] = 2; hdr[12] = (uint8_t)(w & 0xFF); hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)(h & 0xFF); hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32; hdr[17] = 0x28;                       // 左上原点 + 8 位 alpha | upper-left origin, 8-bit alpha
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t row[4096];
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *q = g_rb_map + (uint64_t)y * w * 4u;
        uint32_t x = 0;
        while (x < w) {
            uint32_t c = w - x; if (c > 512u) c = 512u;
            for (uint32_t i = 0; i < c; i++) {
                row[i * 4 + 0] = q[i * 4 + 2];
                row[i * 4 + 1] = q[i * 4 + 1];
                row[i * 4 + 2] = q[i * 4 + 0];
                row[i * 4 + 3] = q[i * 4 + 3];
            }
            fwrite(row, 1, c * 4u, f);
            x += c;
        }
    }
    fclose(f);
    GVL("[vk] RB图已落盘 %{public}s %{public}ux%{public}u", path, w, h);
}

static void rb_report(void)
{
    if (!g_rb_map || !g_rb_w) return;
    uint32_t w = g_rb_w, h = g_rb_h;
    uint64_t n = (uint64_t)w * h, nz = 0, a0 = 0, af = 0;
    unsigned mn[4] = { 255u, 255u, 255u, 255u }, mx[4] = { 0, 0, 0, 0 };
    uint64_t sum[4] = { 0, 0, 0, 0 };
    uint32_t ck = 2166136261u;
    const uint8_t *p = g_rb_map;
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *q = p + i * 4u;
        for (int c = 0; c < 4; c++) { if (q[c] < mn[c]) mn[c] = q[c]; if (q[c] > mx[c]) mx[c] = q[c]; sum[c] += q[c]; }
        if (q[0] | q[1] | q[2]) nz++;
        if (!q[3]) a0++; else if (q[3] == 255) af++;
        if ((i % 53u) == 0) { ck = (ck ^ q[0]) * 16777619u; ck = (ck ^ q[1]) * 16777619u; ck = (ck ^ q[2]) * 16777619u; }
    }
    char rb[192];
    snprintf(rb, sizeof rb, "[vk] RB s=%llu %ux%u nz=%llu/%llu a0=%llu a255=%llu mn=%u,%u,%u,%u mx=%u,%u,%u,%u avg=%u,%u,%u,%u ck=%08x",
             (unsigned long long)g_rb_serial, w, h, (unsigned long long)nz, (unsigned long long)n,
             (unsigned long long)a0, (unsigned long long)af, mn[0], mn[1], mn[2], mn[3],
             mx[0], mx[1], mx[2], mx[3], (unsigned)(sum[0] / n), (unsigned)(sum[1] / n),
             (unsigned)(sum[2] / n), (unsigned)(sum[3] / n), ck);
    GVL("%{public}s", rb);
    rb_ascii();
    rb_tga();
    g_rb_w = 0;
}

// ---------------- 闪屏探针 | flicker probe ----------------
// 报障两则：「过场动画一直闪屏（能看到 kklei）」、「用户许可协议看不清」。截图一张 1.2 秒，逐拍的事抓不到，
// 所以逐拍出图整帧读回，先压成 16x9 的平均亮度网格（一格一个字符，内容一变打一行，两行相减就是「闪在哪一块」）。
// 要看真像素就用 DM_RAW：一轮连拍 4 拍写成裸 RGBA8 文件。第 3 条路（base64 走 hilog）实测废掉：B70 发了 3840 行、
// 原始 hilog 全量落地 88 万行里一条都没有，而同一段时间「闪」行收了 4213 行——日志通道自己吞长行，别再走它。
// Two reports: the cutscene flickers and the EULA text is unreadable. Screenshots cost 1.2 s each, so every present reads the
// whole presented image back: a 16x9 mean-luma hex grid printed once per content change (subtract two lines to see which region
// blinks), and DM_RAW=<n> additionally writes n frames as raw RGBA8 files. The third route - base64 through hilog - is dead:
// B70 emitted 3840 chunk lines, a full raw hilog capture of 881568 lines contained none of them, while 4213 grid lines from the
// same window arrived. Do not ship pixels through the log again.
static int g_flk_left;                    // DM_FLK = 还剩几拍出图要读回（0 = 关）| presents left to probe
static int g_flk_done;                    // 收工行已经打过 | the closing summary has been printed
static uint8_t *g_flk_map; static VkBuffer g_flk_buf; static VkDeviceMemory g_flk_mem; static uint64_t g_flk_cap;
static int g_flk_pend;                    // 这拍的拷贝已录进命令缓冲，等栅栏 | copy recorded, waiting on the fence
static uint64_t g_flk_last; static int g_flk_same;
static uint64_t g_flk_ring[8];            // 最近 8 拍的内容哈希（找周期）| last eight hashes, to find the period
static uint64_t g_flk_hist[10];           // 期 k = 这拍内容和 k 拍前相同 的次数 | how often content matched k presents ago
static uint64_t g_flk_chg, g_flk_tot;
#define FLK_GX 16u
#define FLK_GY 9u
// DM_RAW = 还想落盘几帧；run = 本轮连拍剩余；gap = 距下次触发还有几拍
static int g_raw_left, g_raw_run, g_raw_gap;
static uint64_t g_raw_files;

// 整帧裸文件落进沙箱：hdc 读沙箱要用 /data/app/el2/100/base/<包>/...（应用自己看到的 /data/storage/el2/... 会 permission denied）
// Whole frame to a file. hdc reaches the sandbox only through /data/app/el2/100/base/<pkg>/... - the app's own view,
// /data/storage/el2/..., is denied from the shell, which is why earlier rounds concluded the sandbox was unreadable.
static void raw_save(vktex *v, unsigned w, unsigned h)
{
    uint64_t serial = v->serial;
    static char dir[512]; static int dir_try;
    if (!dir_try) {
        dir_try = 1;
        const char *d = getenv("DM_RAWDIR");
        if (!d || !*d) { const char *s = getenv("DM_SAVE"); if (!s || !*s) return; snprintf(dir, sizeof dir, "%s/raw", s); }
        else snprintf(dir, sizeof dir, "%s", d);
        if (mkdir(dir, 0755) && errno != EEXIST) { GVL("[vk] 回传目录建不出来 %{public}s errno=%{public}d", dir, errno); return; }
        GVL("[vk] 回传整帧写到 %{public}s", dir);
    }
    if (!dir[0] || !g_flk_map) return;
    {   // 一张源图第一次露面时把身份打全：同一个 VkImage 还是两张、什么格式（暗/亮两拍的根因判据）
        // First sighting of a source image: log identity - one VkImage or two, and the format.
        static uint64_t seen[16]; static int seen_n;
        int known = 0;
        for (int i = 0; i < seen_n; i++) if (seen[i] == serial) { known = 1; break; }
        if (!known) {
            if (seen_n < 16) seen[seen_n++] = serial;
            GVL("[vk] 源图 s%{public}llu 图%{public}llu 格%{public}d %{public}ux%{public}u 画过%{public}d 传过%{public}d",
                (unsigned long long)serial, (unsigned long long)(uintptr_t)v->img, (int)v->fmt, v->w, v->h, v->drawn, v->uploaded);
        }
    }
    char path[640];
    snprintf(path, sizeof path, "%s/p%llu_f%llu_%ux%u.rgba", dir,
             (unsigned long long)serial, (unsigned long long)g_presents, w, h);
    FILE *f = fopen(path, "wb");
    if (!f) { GVL("[vk] 回传整帧写不出 %{public}s errno=%{public}d", path, errno); return; }
    fwrite(g_flk_map, 1, (size_t)w * h * 4u, f);
    fclose(f);
    g_raw_files++;
}

static int flk_arm(VkCommandBuffer cb, vktex *v)
{
    uint64_t need = (uint64_t)v->w * v->h * 4u;
    if (v->is_depth || v->fmt != VK_FORMAT_R8G8B8A8_UNORM || v->layers != 1 || v->levels != 1 ||
        v->w < 64u || v->h < 64u || need > (16u << 20)) return 0;
    if (need > g_flk_cap) {   // 出图尺寸一变就得换缓冲：开局 1280x720，进世界后客体可能换档
        if (g_flk_buf) { vkDestroyBuffer(g_dev, g_flk_buf, NULL); g_flk_buf = 0; g_flk_mem = 0; g_flk_map = NULL; }
        g_flk_cap = 0;
        if (!mk_hostbuf(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_flk_buf, &g_flk_mem, (void **)&g_flk_map)) {
            g_flk_buf = 0; g_flk_mem = 0; g_flk_map = NULL; g_flk_left = 0; g_raw_left = 0; g_raw_run = 0; return 0;
        }
        g_flk_cap = need;
        GVL("[vk] 闪屏探针缓冲 %{public}u x %{public}u = %{public}llu MB", v->w, v->h, (unsigned long long)(need >> 20));
    }
    if (!g_flk_map) return 0;
    to_layout(cb, v, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rc.imageExtent = (VkExtent3D){ v->w, v->h, 1 };
    vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_flk_buf, 1, &rc);
    g_flk_pend = 1;
    return 1;
}

static void flk_report(vktex *v, int si)
{
    if (!g_flk_pend) return;
    g_flk_pend = 0;
    uint32_t w = v->w, h = v->h;
    // 16x9 网格：每格平均亮度压成一个十六进制字符。两行相减 = 闪在哪一块；一行全 0 = 黑帧。
    static char grid[FLK_GX * FLK_GY + 1];
    uint64_t hh = 1469598103934663733ULL; uint32_t lit = 0, n = 0;
    for (unsigned cy = 0; cy < FLK_GY; cy++) {
        for (unsigned cx = 0; cx < FLK_GX; cx++) {
            uint64_t s = 0; uint32_t c = 0;
            unsigned y0 = h * cy / FLK_GY, y1 = h * (cy + 1) / FLK_GY;
            unsigned x0 = w * cx / FLK_GX, x1 = w * (cx + 1) / FLK_GX;
            for (unsigned y = y0; y < y1; y += 4) {
                const uint8_t *q = g_flk_map + ((size_t)y * w + x0) * 4u;
                for (unsigned x = x0; x < x1; x++, q += 4) s += (q[0] * 77u + q[1] * 151u + q[2] * 28u) >> 8;
                c += x1 - x0;
            }
            unsigned m = c ? (unsigned)(s / c) : 0;
            grid[cy * FLK_GX + cx] = (char)(m > 240 ? '#' : '0' + (m >> 4));
        }
    }
    grid[FLK_GX * FLK_GY] = 0;
    for (size_t i = 0; i < (size_t)w * h * 4u; i += 256) {
        hh = (hh ^ g_flk_map[i]) * 1099511628211ULL;
        hh = (hh ^ g_flk_map[i + 1]) * 1099511628211ULL;
        hh = (hh ^ g_flk_map[i + 2]) * 1099511628211ULL;
    }
    for (size_t i = 0; i < (size_t)w * h * 4u; i += 64) { n++; if (g_flk_map[i] + g_flk_map[i + 1] + g_flk_map[i + 2] > 380u) lit++; }
    g_flk_tot++;
    if (g_raw_gap > 0) g_raw_gap--;
    int period = 0, changed = (hh != g_flk_last);
    if (changed) {
        for (int k = 1; k <= 8; k++) if (g_flk_ring[k % 8] == hh) { period = k + 1; break; }
        if (period) g_flk_hist[period]++;
        g_flk_chg++;
        GVL("[vk] 闪 出图%{public}llu s%{public}llu 源%{public}d 亮%{public}u%% 同前%{public}d 期%{public}d 帧%{public}d/%{public}d/%{public}d 落%{public}llu 挂%{public}llu 上%{public}llu 画%{public}d 网%{public}s",
            (unsigned long long)g_presents, (unsigned long long)v->serial, si,
            n ? (unsigned)(100u * lit / n) : 0u, g_flk_same, period,
            g_raw_left, g_raw_run, g_raw_gap, (unsigned long long)g_raw_files,
            (unsigned long long)(si >= 0 ? atomic_load(&g_src_att[si]) : 0ULL),
            (unsigned long long)(si >= 0 ? atomic_load(&g_src_pres[si]) : 0ULL), v->drawn, grid);
        g_flk_same = 0;
    } else g_flk_same++;
    // 整帧落盘跟着内容变化走：每出现一个新画面连拍 2 帧（成对的才能看出交替），两次至少隔 150 拍。
    // 静态画面只会落一次，所以静态的 EULA 也正好抓得到——按拍数定周期太费盘，按变化抓一圈就够。
    // Ship frames on content changes: 2 consecutive frames per new picture (a pair is what shows an alternation), at least
    // 150 presents apart. A static screen is therefore sampled exactly once - which is what the EULA needs.
    if (changed && g_raw_left > 0 && !g_raw_run && !g_raw_gap) g_raw_run = 2;
    if (g_raw_left > 0 && g_raw_run > 0) {
        g_raw_left--; g_raw_run--;
        if (!g_raw_run) g_raw_gap = 150;
        raw_save(v, w, h);
    }
    for (int k = 7; k >= 1; k--) g_flk_ring[k] = g_flk_ring[k - 1];
    g_flk_ring[0] = hh; g_flk_last = hh;
    if (!g_flk_left && !g_raw_left && !g_raw_run && !g_flk_done) {
        g_flk_done = 1;
        GVL("[vk] 闪屏探针收工：读回%{public}llu 拍，内容变了%{public}llu 次｜回到 2/3/4/5/6/7/8 拍前 = %{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu｜整帧落盘%{public}llu 张",
            (unsigned long long)g_flk_tot, (unsigned long long)g_flk_chg,
            (unsigned long long)g_flk_hist[2], (unsigned long long)g_flk_hist[3], (unsigned long long)g_flk_hist[4],
            (unsigned long long)g_flk_hist[5], (unsigned long long)g_flk_hist[6], (unsigned long long)g_flk_hist[7],
            (unsigned long long)g_flk_hist[8], (unsigned long long)g_raw_files);
    }
}

// 等到 fence：本刀每次提交内部就等完栅栏（同步提交），走到这里工作已经完成
// Wait up to fence: this cut waits the fence inside mtl_sub_commit, so by the time the guest asks everything is already done.
// 以前在这里等 g_fence 会死等：提交里刚 reset 过它，下一次信号要等下一次提交，而下一次提交只能由这个线程自己发出。
// Waiting g_fence here used to hang: the commit had just reset it, so the next signal required the next submit - which only this very
// thread can issue.
int mtl_wait(uint64_t fence, uint64_t timeout_ns)
{
    (void)fence;
    if (!g_on) return 1;
    // 这里绝不能再排空队列。mtl_sub_commit 已经 vkWaitForFences(UINT64_MAX) 等完自己那条栅栏并 reset，
    // 客体开口问的时候我们提交过的活儿一定已经跑完；而 vkQueueWaitIdle 等的是「整条队列 + 呈现」，
    // 队列里只要挂着一次没兑现的 swapchain acquire（图已经取满、要等下一次呈现才放行），
    // 这条线程就又只能自己去呈现 —— 自锁。第 117/118/119 轮那个「停在第一次游戏通道之前、
    // 关掉重放和提交通道都照停」的形状就是它：现场全是 svc 等回话，且出图线程 == 游戏线程。
    // Never drain the queue here: mtl_sub_commit already waited its own fence, so anything we submitted is done by the time
    // the guest asks. vkQueueWaitIdle waits on the whole queue *and* presentation, so a swapchain acquire still owed a
    // present deadlocks this very thread (present runs on the game thread). Rounds 117-119's stall was exactly that.
    const char *old = getenv("DM_VK_NOWAIT");         // =0 回旧行为（每次都排空），同一构建可做启动参数 A/B | =0 restores the old drain, A/B without a rebuild
    if (old && !strcmp(old, "0")) { mtl_drain(); return 1; }
    return 1;
}

// 同样不能再等 g_fence（提交里已经等完并 reset），排队列排空即可
// Same reason g_fence is not waited here: the commit already waited it and reset it.
void mtl_finish(void) { if (g_on) mtl_drain(); }
void mtl_drain(void) { if (g_on) vkQueueWaitIdle(g_q); }

// ---------------- 出图 | present ----------------
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4])
{
    (void)period_s; (void)crop;      // crop 等满屏三角形那一刀再用（通道里按 UV 取样剪口）| the crop is used once the full-screen triangle lands
    uint64_t ticket = ++g_presents;
    if (!vk_ensure()) return ticket;
    { int tid = (int)syscall(SYS_gettid);
      if (!g_pres_tid) g_pres_tid = tid;
      else if (g_pres_tid != tid && g_pres_tid != -1) g_pres_tid = -1; }
    if (g_skip & 8) return ticket;
    void *win; uint64_t ww = 0, hh = 0;
    if (!dm_ohos_window(&win, &ww, &hh)) return ticket;
    if (!g_sc) { if (!swapchain_build(win, ww, hh)) return ticket; }
    vktex *v = tex_get(t);
    if (!v) return ticket;
    {
        int known = 0;
        for (int i = 0; i < g_src_n; i++) if (g_src_v[i] == v) { known = 1; atomic_fetch_add(&g_src_pres[i], 1); break; }
        if (!known) {
            if (g_src_n < 4) { g_src_v[g_src_n] = v; atomic_fetch_add(&g_src_pres[g_src_n], 1); g_src_n++; }
            GVL("[vk] 出图源 %{public}d serial=%{public}llu NVN格式 %{public}d Vk格式 %{public}d %{public}u x %{public}u 层 %{public}u mip %{public}u",
                g_src_n, (unsigned long long)v->serial, v->d.d.format, (int)v->fmt, v->w, v->h, v->layers, v->levels);
        }
    }
    // 位 1073741824 = 出图永远采第一张出图源：闪屏若是「两张交换链图一张有货一张没货」，钉住一张就该不再闪
    // Bit 1073741824 pins the present to the first source image: if the flicker is one of the two window images being short,
    // pinning makes it stop.
    if ((g_skip & 1073741824) && g_src_n && g_src_v[0] != v) {
        static int saidpin; if (!saidpin++) GVL("[vk] DM_VK_SKIP 位 1073741824：出图钉死采第一张源图 s%{public}llu", (unsigned long long)g_src_v[0]->serial);
        v = g_src_v[0];
    }
    int src_i = -1; for (int i = 0; i < g_src_n; i++) if (g_src_v[i] == v) { src_i = i; break; }
    // 出图不碰游戏的命令缓冲 g_cb：第 49 轮把经典通道录进 g_cb（游戏那条缓冲里刚收过 vkCmdEndRendering 的动态通道），
    // 几十帧后 present = -4。第 51 轮单开一条缓冲仍然 15 帧就 present = -4
    //   ⇒ 混录不是根因；出图仍单开一条缓冲（各自的录制状态干净），锅在别处，用位 64/128 继续拆。
    // Present does not reuse the game's command buffer: recording the classic pass into g_cb (whose dynamic pass was just ended with
    // vkCmdEndRendering) took the device down after a few dozen frames (round 49, present = -4). Round 51 gave the present its own buffer
    // and still answered present = -4 after 15 frames, so the mixed recording was not the root cause - the buffer stays separate anyway
    // (cleaner per-buffer state), and bits 64/128 split the remaining suspects. (When the full-screen triangle arrives this buffer will
    // need a dependency on the game's submit that produced the source texture - sampling stale content is the cost of the split.)
    // 通道没建起来就别 acquire：那条信号量没人等，会一直挂到下一帧 | don't acquire before the pass exists - nobody would consume the signal
    if (!g_sc_rp) {
        static int saidrp;
        if (!saidrp++) GRED("出图通道没建起来（通道 %{public}p）", (void *)(uintptr_t)g_sc_rp);
        return ticket;
    }
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(g_dev, g_sc, UINT64_MAX, g_acq_sem[0], NULL, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        GVL("[vk] acquire = %{public}s → 重建交换链", vr(ar));
        swapchain_destroy();
        if (!swapchain_build(win, ww, hh)) return ticket;
        ar = vkAcquireNextImageKHR(g_dev, g_sc, UINT64_MAX, g_acq_sem[0], NULL, &idx);
    }
    if (ar != VK_SUCCESS) { GRED("acquire = %{public}d (%{public}s)", (int)ar, vr(ar)); return ticket; }

    // 搬像素的方式由实测决定 | how the pixels get moved - decided by measurement, not by guesswork
    // 第 46 轮 DM_VK_SKIP=16（照旧 acquire + 两道交换链屏障 + 提交 + 呈现，只是不碰任何像素）跑满 484 帧全绿
    //   ⇒ 出图的骨架（acquire 信号量 / 出图栅栏 / 交换链布局屏障）本身是干净的。
    // 第 47 轮 只往交换链图上发一条 vkCmdClearColorImage（DM_VK_SKIP=64）照样 present 提交 = -4
    //   ⇒ Maleoon 920 不接受对交换链图做 transfer 写：blit 与 clear 都算，vkQueueSubmit 当场打掉设备。
    // 第 48 轮 改用动态渲染（vkCmdBeginRendering 挂交换链视图）：提交不再报错，但 GPU 侧照样炸——
    //   /system/bin/render_service 的 DDGR::Backend::TimelineWaitThread 报 LastFatalMessage「Fatal: Panic! Unrecoverable GPU error!」自杀，
    //   表面随它消失（此后 acquire = VK_ERROR_SURFACE_LOST_KHR）。所以动态渲染也不能用在交换链图上。
    //   ⇒ 抄 M2-0 冒烟实测绿的写法（dm/ohos/vk_smoke.c:318-433）：经典 VkRenderPass + 每张图一个 VkFramebuffer，
    //     布局转换交给通道自己做（initial UNDEFINED → final PRESENT_SRC），不再手发交换链屏障；
    //     提交用信号量喂 vkQueuePresentKHR，等待阶段是 COLOR_ATTACHMENT_OUTPUT。
    // 本刀通道里只做清色（画面纯色品红）；等 SPIR-V 管线那一刀到位，就在同一个通道里画满屏三角形把源图贴上来。
    // Round 46, DM_VK_SKIP=16 (acquire + both swapchain barriers + submit + present, moving no pixels) ran 484 frames entirely green
    //   => the present skeleton itself is clean. Round 47, a single vkCmdClearColorImage onto the swapchain image still answers -4
    //   => Maleoon 920 refuses transfer writes to swapchain images. Round 48, dynamic rendering onto the swapchain view submits fine but
    //   the GPU faults anyway: render_service's DDGR TimelineWaitThread panics with "Unrecoverable GPU error!" and dies, and the surface
    //   goes with it (acquire = SURFACE_LOST afterwards). So the swapchain path copies the recipe M2-0 proved green in dm/ohos/vk_smoke.c:
    //   a classic VkRenderPass with one VkFramebuffer per image, layout transitions done by the pass itself (UNDEFINED -> PRESENT_SRC),
    //   no hand-written swapchain barriers, and a timeline semaphore feeding vkQueuePresentKHR with a COLOR_ATTACHMENT_OUTPUT wait.
    // This cut only clears inside the pass (flat magenta); the full-screen triangle sampling the source comes with the pipeline cut.
    if (idx >= g_sc_n || !g_sc_fb[idx]) {
        static int saidfb;
        if (!saidfb++) GRED("出图帧缓冲缺失（号 %{public}u / 链 %{public}u 张）", idx, g_sc_n);
        return ticket;
    }
    // 出图专属缓冲：一次提交一条，录完即弃 | the present's own buffer - allocated, recorded and freed per submission
    VkCommandBuffer pcb = NULL;
    VkCommandBufferAllocateInfo pca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    pca.commandPool = g_cp;
    pca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    pca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_dev, &pca, &pcb) != VK_SUCCESS) { GRED("分配出图命令缓冲失败"); return ticket; }   // 1.0 头里没有 vkReleaseSemaphore，这条 OOM 路只能把信号留给下一帧
    VkCommandBufferBeginInfo pbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    pbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(pcb, &pbi);
    if (g_skip & 16) { static int s16; if (!s16++) GVL("[vk] DM_VK_SKIP 位 16：出图不搬像素"); }
    else {
        // 位 512 = 通道写离屏图，不写交换链那张（present 仍然照走，画面是旧内容）
        VkFramebuffer fb_use = (g_skip & 512 && g_off_fb) ? g_off_fb : g_sc_fb[idx];
        if ((g_skip & 512) && g_off_fb) { static int s512; if (!s512++) GVL("[vk] DM_VK_SKIP 位 512：出图通道写离屏图 %{public}u x %{public}u，交换链那张不碰", g_sc_ext.width, g_sc_ext.height); }
        VkClearValue cv = { .color = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } } };
        // 通道外先把源图准备好（布局转换 + 描述符集），拿不到就这一帧只清色
        // Set the source up before entering the pass (layout + descriptor set); without it this frame stays a plain clear.
        int want_rb = g_rb_on && (g_presents % DUMP_EVERY) == 1;
        if (want_rb) {
            // 第 95 轮：节拍 60 与三缓冲的 3 同余，五次窗口全落在同一张（本帧上屏恒 serial=5），轮转读别的张没意义。
            // 直接读本帧真正上屏的那张：它有没有内容 = 屏幕该有什么。挂过通道的张数、每张上屏次数在下面一行。
            // Round 95: a 60-frame window is congruent with the game's triple buffering, so all five windows landed on the same
            // source (serial 5 every time). Rotating over the other two measured images nobody presented. Read the one being shown.
            // 三拍一轮：0 = 本帧上屏那张，1/2 = 上屏通道采的两张真图。第 97 轮实测上屏那张 RGB 恒 0、只有 alpha，
            // 要分清是「游戏喂进来的输入就是黑的」还是「我们把它自己的输出弄丢了」。
            // Three-beat rotation: 0 = the image being presented, 1/2 = the two real textures that pass samples.
            // Round 97 measured RGB exactly 0 (alpha only) on the presented image - this tells an empty input from a lost output.
            uint64_t win = g_presents / DUMP_EVERY;
            vktex *rbv = v; const char *rbtag = "上屏";
            if (win % 3 == 1 && g_sp_tex[0]) { rbv = g_sp_tex[0]; rbtag = "输入1"; }
            else if (win % 3 == 2 && g_sp_tex[1]) { rbv = g_sp_tex[1]; rbtag = "输入2"; }
            GVL("[vk] RB 窗口 %{public}s：本帧上屏 serial=%{public}llu 挂通道%{public}llu 上屏%{public}llu 画过%{public}d 传过%{public}d 布局%{public}d｜读 %{public}s serial=%{public}llu %{public}ux%{public}u 格%{public}d",
                rbtag, (unsigned long long)v->serial,
                (unsigned long long)(src_i >= 0 ? atomic_load(&g_src_att[src_i]) : 0ULL),
                (unsigned long long)(src_i >= 0 ? atomic_load(&g_src_pres[src_i]) : 0ULL),
                v->drawn, v->uploaded, (int)v->layout,
                rbtag, (unsigned long long)rbv->serial, rbv->w, rbv->h, (int)rbv->fmt);
            rb_arm(pcb, rbv);                                       // 里已经把源图转到 TRANSFER_SRC，随后 tri_prep 再转到可采样
        }
        // 闪屏探针：DM_FLK=<拍数> 逐拍出图整帧读回（16x9 网格一行 + 变化周期）；DM_RAW=<帧数> 每 500 拍一轮 4 帧，整帧裸文件落盘
        // Flicker probe: DM_FLK=<n> reads the whole presented image back per present (a 16x9 hex grid line + the blink period);
        // DM_RAW=<n> additionally writes n frames as raw RGBA8 files, 4 per round, one round every 500 presents.
        if (g_flk_left > 0 || g_raw_left > 0 || g_raw_run > 0) { if (g_flk_left > 0) g_flk_left--; flk_arm(pcb, v); }
        VkDescriptorSet triset = tri_prep(pcb, v);
        VkRenderPassBeginInfo rpb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpb.renderPass = g_sc_rp;
        rpb.framebuffer = fb_use;
        rpb.renderArea = (VkRect2D){ { 0, 0 }, g_sc_ext };
        rpb.clearValueCount = 1;
        rpb.pClearValues = &cv;
        vkCmdBeginRenderPass(pcb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
        if (triset) tri_emit(pcb, triset);
        vkCmdEndRenderPass(pcb);
    }
    vkEndCommandBuffer(pcb);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    si.waitSemaphoreCount = 1;
    // 必须给等待阶段：waitSemaphoreCount=1 而 pWaitDstStageMask=NULL ⇒ 驱动在 vkQueueSubmit 里空指针
    // （第 37 轮的现场：mtl_present+1516 → libmaleoon_v200.so SEGV_MAPERR@0）。
    // The wait stage is mandatory: waitSemaphoreCount=1 with pWaitDstStageMask=NULL makes the driver dereference NULL inside
    // vkQueueSubmit (round 37: mtl_present+1516 -> libmaleoon_v200.so, SEGV_MAPERR@0).
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &pcb;
    si.pWaitSemaphores = &g_acq_sem[0];   // acquire 用的就是 0 号（每张图共用一条二进制信号量，一次信号配一次等待）| acquire signals slot 0 too, so wait that one - one signal, one wait
    // 提交前栅栏必须处于未 signaled 状态（VUID-vkQueueSubmit-fence-00064）：本函数末尾每次都等过它，
    // 复用即违规，先把 reset 补上。
    // The fence must be unsignaled when submitted (VUID-vkQueueSubmit-fence-00064): this function waits it at the bottom of every call, so reset it.
    if (g_pfence) vkResetFences(g_dev, 1, &g_pfence);
    // 位 64 = 通道照录（真往交换链图上写色），但呈现不走可见信号量：改成等出图栅栏后再 present。
    // 第 51 轮把「录通道」和「用 g_vis_sem」两件事一起开，present = -4；第 50 轮位 16（既不录通道也不用信号量）615 帧全绿
    // ⇒ 这两个变量必须拆开测：本位单测可见信号量这条链。
    // Bit 64 = record the pass (real writes to the swapchain image) but present without the visible semaphore: wait the present fence
    // instead. Round 51 turned both the pass and g_vis_sem on together and answered present = -4, while round 50 (bit 16 - neither) ran
    // 615 frames green, so the two variables have to be measured apart.
    int do_vis = !(g_skip & (16 | 32 | 64));
    if (do_vis) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_vis_sem; }
    int do_wait = !(g_skip & 128);        // 位 128 = 出图提交前把队列排空：测「游戏的动态渲染提交与出图的经典通道提交并发」是不是雷区
                                          // bit 128 = drain the queue before the present submit: tests whether the game's dynamic-render
                                          // submissions running concurrently with our classic pass is what faults the GPU
    VkResult sr;
    if (!do_wait) vkQueueWaitIdle(g_q);   // 位 128：先让队列空下来，出图这条提交就不与游戏的任何提交同时在飞
    sr = vkQueueSubmit(g_q, 1, &si, g_pfence);
    if (sr != VK_SUCCESS) {
        GRED("present 提交 = %{public}d (%{public}s)｜tid 出图 %{public}d 游戏提交 %{public}d", (int)sr, vr(sr), g_pres_tid, g_sub_tid);
        buf_ops_dump("出图");
        // 出图独有的现场：源图身份 + 交换链那张。没有校验层，只能自己把参数摊开看
        // The scene unique to present: the source image and the swapchain. No validation layer, so print the arguments ourselves.
        GRED("出图源图：serial=%{public}llu NVN格式 %{public}d Vk格式 %{public}d %{public}u x %{public}u 层 %{public}u mip %{public}u 深度 %{public}d 记录布局 %{public}d | 链 %{public}u 张 %{public}u x %{public}u 格式 %{public}d 号 %{public}u",
             (unsigned long long)v->serial, v->d.d.format, (int)v->fmt, v->w, v->h, v->layers, v->levels, v->is_depth, (int)v->layout,
             g_sc_n, g_sc_ext.width, g_sc_ext.height, (int)g_sc_fmt, idx);
    }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.swapchainCount = 1;
    pi.pImageIndices = &idx;
    pi.pSwapchains = &g_sc;
    VkResult pr = VK_SUCCESS;
    if (sr == VK_SUCCESS && do_vis) { pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_vis_sem; }   // 冒烟同款：出图等渲染那一次信号 | as in the smoke test: present waits the render's signal
    // 位 64/16 这类不用信号量的走法：present 前先把出图栅栏等下来，保证色真写完了再交出去
    // Without the visible semaphore, wait the present fence first so the color really landed before handing the image over.
    if (sr == VK_SUCCESS && !do_vis && g_pfence) vkWaitForFences(g_dev, 1, &g_pfence, VK_TRUE, UINT64_MAX);
    if (g_skip & 32) { static int said32; if (!said32++) GVL("[vk] DM_VK_SKIP 位 32：只提交，不调 vkQueuePresentKHR"); }
    else if (sr == VK_SUCCESS) pr = vkQueuePresentKHR(g_q, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR) GRED("present = %{public}d (%{public}s)", (int)pr, vr(pr));
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) { swapchain_destroy(); }
    // 出图等自己的栅栏，等完才回收缓冲：同游戏提交那条理由（缓冲还在飞就 free = 把驱动在用的内存放回池里重用）
    // Wait our own fence before freeing: same reason as the game submission - freeing a buffer that is still executing returns memory
    // the driver is using back to the pool. This cut makes present synchronous, so g_pfence is also idle when it is reused next frame.
    if (g_pfence && sr == VK_SUCCESS) vkWaitForFences(g_dev, 1, &g_pfence, VK_TRUE, UINT64_MAX);
    rb_report();          // 栅栏落地 = 那条拷贝执行完了（同一队列）| the fence means the copy ran: same queue
    flk_report(v, src_i);
    vkFreeCommandBuffers(g_dev, g_cp, 1, &pcb);
    if ((g_presents % DUMP_EVERY) == 1) GVL("[vk] 出图 %{public}llu 帧（通道 %{public}llu、清 %{public}llu、贴图 %{public}llu 张、提交 %{public}llu）tid 出图 %{public}d 游戏 %{public}d",
                                     (unsigned long long)g_presents, (unsigned long long)atomic_load(&g_passes), (unsigned long long)atomic_load(&g_clears),
                                     (unsigned long long)g_tex_new, (unsigned long long)g_submits, g_pres_tid, g_sub_tid);
    if ((g_presents % DUMP_EVERY) == 1) vk_probe_dump();   // 同一节拍把绘制侧的记账打出来 | same cadence, dump the draw-side bookkeeping
    return ticket;
}

void mtl_present_wait(uint64_t ticket) { (void)ticket; mtl_finish(); }
void mtl_present_flush(void) { mtl_finish(); }
void mtl_present_test_stall(uint64_t ns) { (void)ns; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// ---------------- 命令回放 | command replay ----------------
// 递归版在真机上炸了：D2R 的子列表是链式的（一条列表 call 下一条），实测嵌套 250+ 层，
// 客体线程栈只有几百 KB，第 250 层再进 Maleoon 驱动就踩到栈护栏（SEGV_ACCERR @stack low）。
// 改成显式返回栈：一条句柄只用一帧 C 栈，深度上限由我们自己控，超了就截断并报一次。
// The recursive version crashed on device: D2R's sub-lists are chained (each list CALLs the next), measured 250+ deep,
// and the guest thread's stack is only a few hundred KB - at depth 250 the next Maleoon driver call hit the stack guard (SEGV_ACCERR @stack low).
// Now an explicit return stack: one handle costs one C frame, the depth cap is ours, and exceeding it truncates with a single report.
#define EXEC_DEPTH 1024
static _Thread_local const uint8_t *g_ret[EXEC_DEPTH];
static _Thread_local int g_ret_n;

// ---------------- 着色器键 + 绘制记账探针（M2-3 前置：只算不画，一条 vk 都不发）----------------
// Shader key + draw bookkeeping (the prerequisite for M2-3: it computes, it draws nothing - zero vk calls)
// 键 = sha256(控制段前 0x838 字节 + 该段指令区前 0x838 字节) 的前 16 位十六进制。
// 为什么不是整 0x880（B58/B59 对拍，2026-10-17）：控制段 [0x838,0x880) 不是指令区，是一段每实例可变的浮点区，
// 同一个程序同一次开机里就能落出 3 个不同键（639f989b→f2c79691/6e951075/be0adaef，码字节逐字节相同），
// 而且换个包（.so 或 shaders.bin 大小一变，客体堆整体平移 0x20000）全部键作废 ⇒ 出库永远追不上。
// 22 对同码样本里 0x838 之前一字节不差，所以按「稳定前缀 + 码」出题：跨包 19/19 键对得上，且没有把不同码并成一个键。
// 离线链 ds_shbin.py 用同一条式子重算键，两边一起改。
// Key = first 16 hex digits of sha256(first 0x838 bytes of the control segment + first 0x838 bytes of that stage's instruction area).
// Why not the whole 0x880 (B58/B59 cross-check, 2026-10-17): [0x838,0x880) is not the code head but per-instance mutable float data -
// one program produced 3 different keys within a single boot (639f989b -> f2c79691/6e951075/be0adaef, byte-identical code),
// and any package change (guest heap slides 0x20000) invalidated every key, so the offline library could never catch up.
// Across 22 same-code pairs nothing before 0x838 differed, so hashing "stable prefix + code" matches 19/19 keys across packages
// without merging different code streams into one key. ds_shbin.py recomputes the key with the same formula - both sides change together.
#define SH_STAGE_N 6
#define CTRL_BYTES 0x880
#define CTRL_STABLE 0x838    // 控制段里真正属于「这个程序」的前缀；[0x838,0x880) 是每实例可变的浮点区 | the part of the control segment that identifies the program; [0x838,0x880) is per-instance mutable float data
#define KEY_CODE_MAX CTRL_STABLE   // 参与哈希的指令区上限 | how many instruction bytes go into the hash
typedef struct {
    uint64_t serial;                    // prog_t.serial：内容一变号就变，哈希因此只算一次 | content change bumps the serial, so hashing happens once
    int nstage;
    char key[SH_STAGE_N][17];           // 空串 = 这一段没算出来（指针不可读）| empty = not computed (control pointer unreadable)
    uint64_t binds, draws;
    int vi, fi;                         // 段键对 shaders.bin 里的条目号（-1 = 库里没有）| entry indices for this key pair (-1 = not in the library)
    uint8_t resolved;                   // 已对着库解过一次（库没就绪时下次绑定再试）| resolved against the library (retry on the next bind while it is not ready)
} progrec;
#define PROG_MAX 512
static progrec g_progs[PROG_MAX];
static int g_nprogs;
static progrec *g_cur_prog;             // 本句柄当前绑定的程序，BEGIN 清掉 | program bound in this handle, cleared at BEGIN
static int g_bind_vi = -1, g_bind_fi = -1;   // 本条录制里已绑过的半边（客体一段一个程序时用它凑成对）| the half already bound in this recording (used to complete a pair when the guest binds one stage per program)
static _Atomic uint64_t g_ctrl_bad, g_draw_noprog;
static uint32_t g_prim_hist[32];        // OP_DRAW 的拓扑直方图 | primitive topology histogram of OP_DRAW
static uint32_t g_itype_hist[8];        // OP_DRAW_IDX 的索引类型直方图 | index type histogram of OP_DRAW_IDX
#define SHKEY_MAX 512
static char g_shkeys[SHKEY_MAX][17];    // 去重后的全部段键：离线构建机的输入清单 | all stage keys, deduplicated: the input list for the offline build
static int g_nshkeys;

static int pages_readable(const void *p, size_t n)
{
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)0xFFF, end = (uintptr_t)p + n;
    for (; a < end; a += 4096) if (!host_writable((const void *)a)) return 0;
    return 1;
}

static void sh_key_of(const void *ctrl, uint64_t code_addr, size_t klen, char out[17])
{
    unsigned char md[32];
    static const char hx[] = "0123456789abcdef";
    uint8_t buf[CTRL_STABLE + KEY_CODE_MAX];
    memcpy(buf, (const unsigned char *)ctrl, CTRL_STABLE);
    memcpy(buf + CTRL_STABLE, (const void *)(uintptr_t)code_addr, klen);
    CC_SHA256(buf, (CC_LONG)(CTRL_STABLE + klen), md);
    for (int i = 0; i < 8; i++) { out[2 * i] = hx[md[i] >> 4]; out[2 * i + 1] = hx[md[i] & 15]; }
    out[16] = 0;
}

// 每个新段键单独一行：这一行的集合就是离线 SPIR-V 构建的输入清单（第 66 轮的 40 个上限漏掉了绘制最多的程序）
// One line per new stage key: the set of these lines is the input list for the offline SPIR-V build (round 66's 40-entry cap missed the most-drawn program)
// DM_VK_KEYDUMP=<沙箱绝对路径>：每个新键把**客体内存里那份控制段（0x880）+ 它的指令区**原样落盘，离线翻译就以这份为准。
// 为什么非要落盘（B20 实测）：饥荒的控制段真身只有 0x838 字节（D2R 是 0x880），文件侧的 .ksh 在 0x838 之后接的是本段自己的指令区，
// 逐字节比对：前 0x838 一致，差异全落在 +0x838 之后 ⇒ 拿 romfs 的 shaders_nx/*.ksh 算出来的键对不上设备。
// 键式换到「前缀 + 码」（见上）后这条差异不再是问题，但落盘仍然是出题依据：翻译器要的就是这份被装载改写过的指令区。
// DM_VK_KEYDUMP=<absolute sandbox path>: for every new key, dump the control segment (0x880) and its instruction area exactly as they sit in guest memory - the offline translator builds from this copy.
// Why a dump was mandatory (measured on B20): Don't Starve's real control segment is only 0x838 bytes (D2R uses 0x880), and in a .ksh file what follows those 0x838 bytes is this stage's own instruction area.
// Byte-for-byte: the first 0x838 match, every difference lies past +0x838 => keys computed from the romfs shaders_nx/*.ksh never matched the device.
// After moving the key to "stable prefix + code" that gap no longer breaks matching, but the dump stays the source of truth: it is the loader-patched instruction area the translator needs.
// 记录 = 32 字节头（键 16 / 段号 4 / 码地址 8 / 码长 4，小端）+ 0x880 控制段 + 码长字节指令区。
static void key_note(int stage, const char *k, const void *ctrl, uint64_t code_addr)
{
    for (int i = 0; i < g_nshkeys; i++) if (!strcmp(g_shkeys[i], k)) return;
    if (g_nshkeys >= SHKEY_MAX) return;
    strcpy(g_shkeys[g_nshkeys++], k);
    const unsigned char *c8 = (const unsigned char *)ctrl;
    uint32_t tot = ((const uint32_t *)(c8 + 0x704))[0];   // 控制段 +0x704：指令区总长（D2R 的出包脚本同一口径）
    const unsigned char *cd = (const unsigned char *)(uintptr_t)code_addr;
    uint32_t cl = (tot >= 0x40 && tot <= 0x20000) ? tot : 0x4000;
    if (!code_addr || !pages_readable(cd, 0x40)) cl = 0;
    else while (cl > 0x40 && !pages_readable(cd, cl)) cl -= 0x40;
    GVL("[vk] 键 %{public}s 段 %{public}d 控制段@%{public}p 码 %{public}#llx 码长 %{public}u 头 %{public}02x %{public}02x %{public}02x %{public}02x",
        k, stage, ctrl, (unsigned long long)code_addr, cl, c8[0], c8[1], c8[2], c8[3]);
    { static const char *kp; static FILE *kf;
      if (!kp) { kp = getenv("DM_VK_KEYDUMP"); if (!kp) kp = ""; }
      if (kp[0] && !kf) { kf = fopen(kp, "wb"); if (!kf) GRED("DM_VK_KEYDUMP 打不开 %{public}s：控制段样本不落盘", kp); }
      if (kf) {
          unsigned char h[32]; int i;
          for (i = 0; i < 16; i++) h[i] = (unsigned char)k[i];
          for (i = 0; i < 4; i++) h[16 + i] = (unsigned char)(stage >> (8 * i));
          for (i = 0; i < 8; i++) h[20 + i] = (unsigned char)(code_addr >> (8 * i));
          for (i = 0; i < 4; i++) h[28 + i] = (unsigned char)(cl >> (8 * i));
          fwrite(h, 1, sizeof h, kf); fwrite(ctrl, 1, CTRL_BYTES, kf);
          if (cl) fwrite(cd, 1, cl, kf);
          fflush(kf);
      } }
}

static void do_bind_program(const p_bind_program *r)
{
    const prog_t *p = r->prog;
    if (!p || p->magic != MAGIC('P', 'R', 'O', 'G') || !pages_readable(p, sizeof *p)) { g_cur_prog = NULL; return; }
    int ns = p->nstage;
    if (ns < 1) ns = 1;
    if (ns > SH_STAGE_N) ns = SH_STAGE_N;
    progrec *e = NULL;
    for (int i = 0; i < g_nprogs; i++) if (g_progs[i].serial == p->serial) { e = &g_progs[i]; break; }
    if (!e) {
        if (g_nprogs >= PROG_MAX) { GRED("程序表满了（%{public}d 个）：新程序只记数不建键", PROG_MAX); g_cur_prog = NULL; return; }
        e = &g_progs[g_nprogs++];
        memset(e, 0, sizeof *e);
        e->serial = p->serial;
        e->nstage = ns;
        e->vi = e->fi = -1;
        for (int i = 0; i < ns; i++) {
            const void *c = p->sd[i].control;
            uint64_t cad = (uint64_t)p->sd[i].data;
            if (!pages_readable(c, CTRL_BYTES) || !cad || !pages_readable((const void *)cad, 0x40)) { atomic_fetch_add(&g_ctrl_bad, 1); continue; }
            {   // 出题口径必须和离线 ds_shbin.py 逐字节一致：tot = 控制段 +0x704（越界当 0x4000）、封顶 0x838，不做可读性退让；
                // 退让会让两边算出不同长度，宁可这段没键（计数照旧）也不要一个对不上的键。
                // The recipe must match ds_shbin.py byte for byte: tot = control segment +0x704 (out of range means 0x4000), capped at 0x838, no readability give-back;
                // giving ground would make the two sides hash different lengths - better no key for this stage (still counted) than a key that cannot match.
                uint32_t tot = ((const uint32_t *)((const unsigned char *)c + 0x704))[0];
                if (tot < 0x40 || tot > 0x20000) tot = 0x4000;
                if (tot > KEY_CODE_MAX) tot = KEY_CODE_MAX;
                if (!pages_readable((const void *)cad, tot)) { atomic_fetch_add(&g_ctrl_bad, 1); continue; }
                sh_key_of(c, cad, tot, e->key[i]);
            }
            key_note(i, e->key[i], c, cad);
        }
        // 每个新程序一行（不去重）：客体一次给几段、段序是什么，只能这样问它自己
        // One line per new program (never deduplicated): how many stages the guest passes at once and in which order can only be asked of the guest itself
        { char kb[120]; int o = 0;
          for (int i = 0; i < ns; i++) o += snprintf(kb + o, sizeof kb - o, "%s%s", i ? " " : "", e->key[i][0] ? e->key[i] : "-");
          GVL("[vk] 新程序 序 %{public}#llx 段数 %{public}d 键 %{public}s", (unsigned long long)p->serial, ns, kb); }
    }
    e->binds++;
    // 段键对先解一次：库这时可能还没 mmap（首次绘制才懒加载），没就绪就下次绑定再试
    // Try to resolve the key pair once: the library may not be mapped yet (it loads lazily at the first draw), so retry on a later bind.
    if (!e->resolved && shb_pair(e->key[0], e->key[1], &e->vi, &e->fi)) e->resolved = 1;
    // 饥荒（B22 实测）：客体一次只给一段（段数 1），先绑顶点程序、再绑片元程序，绘制记在后一次上
    // ⇒ 单段程序的那另一半从「本条录制里刚绑过的那半」补上；D2R 两段的走上面那条，不受影响
    // Don't Starve (measured on B22): the guest passes one stage per program (段数 1) - it binds the vertex program, then the fragment one, and draws count against the latter
    // => for a single-stage program the missing half is taken from "the half just bound in this recording"; the two-stage case above is untouched (D2R).
    if (ns == 1 && e->key[0][0] && (e->vi < 0 || e->fi < 0)) {
        int vi = -1, fi = -1;
        if (shb_stage(e->key[0], &vi, &fi)) {
            if (vi >= 0) { e->vi = vi; if (g_bind_fi >= 0) e->fi = g_bind_fi; g_bind_vi = vi; }
            else if (fi >= 0) { e->fi = fi; if (g_bind_vi >= 0) e->vi = g_bind_vi; g_bind_fi = fi; }
        }
    } else if (e->vi >= 0 && e->fi >= 0) { g_bind_vi = e->vi; g_bind_fi = e->fi; }
    g_cur_prog = e;
}

static int g_progs_resolved(void)   // 段键对在库里齐了的程序数：这刀能不能画下去的直接读数 | programs whose vertex+fragment pair is in the library
{
    int n = 0;
    for (int i = 0; i < g_nprogs; i++) if (g_progs[i].vi >= 0 && g_progs[i].fi >= 0) n++;
    return n;
}

static void do_draw_count(int prim, int itype)
{
    if (prim >= 0 && prim < 32) g_prim_hist[prim]++;
    if (itype >= 0 && itype < 8) g_itype_hist[itype]++;
    if (g_cur_prog) g_cur_prog->draws++;
    else atomic_fetch_add(&g_draw_noprog, 1);
}

// 汇总打印：op 直方图 + 最活跃的 3 个程序键 + 拓扑分布。挂在出图的周期线上（mtl_present），
// 因为 mtl_res_counts 那条 tick 在这条路上从来不响（第 65 轮实测：日志里一条「资源」都没有）。
// Summary dump: op histogram + the three most active program keys + the topology spread. Hung off the periodic present line (mtl_present),
// because mtl_res_counts' tick never fires on this path (round 65 measured: not one 资源 line in the log).
static _Atomic uint64_t g_np_out, g_np_nocol, g_np_norps;   // 计数在 do_draw 上面声明、下面用：汇总比绘制早
static _Atomic uint64_t g_dr_dsonly;   // 只挂深度的通道里真重放了多少笔（第 88 轮放行）| how many draws replayed into a depth-only pass (allowed from round 88)

static void vk_probe_dump(void)
{
    char line[1024];
    // 注意：这里是 snprintf 拼字符串，不是 hilog 格式串，用普通 %d/%s（%{public} 只属于 OH_LOG_*）
    // snprintf buffer, not a hilog format string: plain %d/%s (%{public} belongs to OH_LOG_* only)
    int q = snprintf(line, sizeof line, "[vk] op:");
    for (int i = 0; i < OP_COUNT_ && q < (int)sizeof line - 24; i++)
        if (atomic_load(&g_ops[i])) q += snprintf(line + q, sizeof line - q, " %d:%llu", i, (unsigned long long)atomic_load(&g_ops[i]));
    GVL("%{public}s", line);

    int top[3] = { -1, -1, -1 };   // 按 draws 取前 3：一帧里的绘制绝大多数集中在少数程序上 | top 3 by draw count
    for (int i = 0; i < g_nprogs; i++) {
        for (int s = 0; s < 3; s++) {
            if (top[s] < 0 || g_progs[i].draws > g_progs[top[s]].draws) {
                for (int t = 2; t > s; t--) top[t] = top[t - 1];
                top[s] = i;
                break;
            }
        }
    }
    int m = snprintf(line, sizeof line, "[vk] 程序 %d 个、唯一键 %d 个、可解程序对 %d、坏控制段 %llu、无程序绘制 %llu：",
                     g_nprogs, g_nshkeys, g_progs_resolved(), (unsigned long long)atomic_load(&g_ctrl_bad), (unsigned long long)atomic_load(&g_draw_noprog));
    for (int s = 0; s < 3 && top[s] >= 0 && m < (int)sizeof line - 40; s++)
        m += snprintf(line + m, sizeof line - m, " [%s 绑%llu 画%llu]",
                      g_progs[top[s]].key[0][0] ? g_progs[top[s]].key[0] : "无键",
                      (unsigned long long)g_progs[top[s]].binds, (unsigned long long)g_progs[top[s]].draws);
    m += snprintf(line + m, sizeof line - m, "；拓扑");
    for (int i = 0; i < 32 && m < (int)sizeof line - 12; i++) if (g_prim_hist[i]) m += snprintf(line + m, sizeof line - m, " p%d:%u", i, g_prim_hist[i]);
    for (int i = 0; i < 8 && m < (int)sizeof line - 12; i++) if (g_itype_hist[i]) m += snprintf(line + m, sizeof line - m, " i%d:%u", i, g_itype_hist[i]);
    GVL("%{public}s", line);
    {   // 绘制重放单独一行：上一行已被 op 直方图占了大半，拼在一起会被截掉 | its own line: the op histogram already fills most of the buffer, so appending there truncates
        char ds[1280]; ds[0] = 0;
        draw_stats_line(ds, 0, (int)sizeof ds);
        GVL("%{public}s", ds);
        {   // 空图清单单独一行（纯 ASCII）：一次性现场行会被 hilog 环形缓冲挤掉，只有跟着周期行走得出去
            char el[640]; el[0] = 0;
            emp_line(el, (int)sizeof el);
            GVL("%{public}s", el);
        }
        {   // 汇总行太长会被 hilog 截，「无通道」的三类拆单独一行 | its own short line, the long summary gets truncated
            char nl[200];
            // 这里的 %{public} 是二次格式化的占位符：snprintf 认不得它，musl 直接返回 -1 并把已经写进去的那半截截断
            // ——第 87 轮这行只有「out=」就是这件事（值全丢了）。先拼纯 ASCII 串，再交给 GVL 打。
            // %{public} is an hilog marker, not a printf conversion: snprintf fails (returns -1) and truncates what it already wrote,
            // which is why round 87 logged just "out=". Build the plain string first, then hand it to GVL.
            snprintf(nl, sizeof nl, "[vk] NOPASS out=%llu nocol=%llu norp=%llu 上屏绘=%llu 只深度绘=%llu",
                     (unsigned long long)atomic_load(&g_np_out), (unsigned long long)atomic_load(&g_np_nocol),
                     (unsigned long long)atomic_load(&g_np_norps), (unsigned long long)atomic_load(&g_sp_draw),
                     (unsigned long long)atomic_load(&g_dr_dsonly));
            GVL("%{public}s", nl);
        }
        pipe_fail_dump();   // 每条管线失败一行纯 ASCII，短的捞得到 | one short ASCII line per failed pipeline - those survive the grep
    }
    {   // 出图源那一问：游戏到底有没有往「上屏那张」画 | the present-source question: does the game draw onto the on-screen image at all?
        char rs[256];
        int q = snprintf(rs, sizeof rs, "[vk] 源 %d 张 挂上通道 %llu 次｜上屏通道画 %llu 重放 %llu 因%d｜上屏绑定 真%llu 哑%llu｜尺寸",
                         g_src_n, (unsigned long long)atomic_load(&g_rt_src_pass),
                         (unsigned long long)atomic_load(&g_sp_draw), (unsigned long long)atomic_load(&g_sp_replay), g_sp_why,
                         (unsigned long long)atomic_load(&g_spb_real), (unsigned long long)atomic_load(&g_spb_dummy));
        for (int i = 0; i < 8 && q < (int)sizeof rs - 24; i++)
            if (g_rtsize[i].w) q += snprintf(rs + q, sizeof rs - q, " %ux%u:%llu", g_rtsize[i].w, g_rtsize[i].h, (unsigned long long)atomic_load(&g_rtsize[i].n));
        GVL("%{public}s", rs);
        q = snprintf(rs, sizeof rs, "[vk] 出图源各张 挂通道/上屏");
        for (int i = 0; i < g_src_n && q < (int)sizeof rs - 28; i++)
            q += snprintf(rs + q, sizeof rs - q, " %d(s%llu)=%llu/%llu", i,
                          g_src_v[i] ? (unsigned long long)g_src_v[i]->serial : 0ULL,
                          (unsigned long long)atomic_load(&g_src_att[i]),
                          (unsigned long long)atomic_load(&g_src_pres[i]));
        GVL("%{public}s", rs);
        q = snprintf(rs, sizeof rs, "[vk] 画进「输入1」：通道%llu 趟 重放%llu 绘｜输入1 s%llu 画过%d 传过%d｜输入2 s%llu",
                     (unsigned long long)atomic_load(&g_in1_pass), (unsigned long long)atomic_load(&g_in1_draw),
                     g_sp_tex[0] ? (unsigned long long)g_sp_tex[0]->serial : 0ULL,
                     g_sp_tex[0] ? g_sp_tex[0]->drawn : -1, g_sp_tex[0] ? g_sp_tex[0]->uploaded : -1,
                     g_sp_tex[1] ? (unsigned long long)g_sp_tex[1]->serial : 0ULL);
        GVL("%{public}s", rs);
        int p = snprintf(rs, sizeof rs, "[vk] 新建贴图格式");
        for (int i = 0; i < 12 && p < (int)sizeof rs - 20; i++)
            if (g_txfmt[i].n) p += snprintf(rs + p, sizeof rs - p, " f%d:%llu", g_txfmt[i].f, (unsigned long long)atomic_load(&g_txfmt[i].n));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 新建贴图格式2");
        for (int i = 12; i < 24 && p < (int)sizeof rs - 20; i++)
            if (g_txfmt[i].n) p += snprintf(rs + p, sizeof rs - p, " f%d:%llu", g_txfmt[i].f, (unsigned long long)atomic_load(&g_txfmt[i].n));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 游戏填槽 阶段:槽=次数");
        for (int st = 0; st < 6 && p < (int)sizeof rs - 24; st++)
            for (int k = 0; k < 32 && p < (int)sizeof rs - 24; k++)
                if (g_slot_fill[st][k]) p += snprintf(rs + p, sizeof rs - p, " %d:%d=%llu", st, k, (unsigned long long)atomic_load(&g_slot_fill[st][k]));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 游戏填采样槽 阶段:槽=次数");
        for (int st = 0; st < 6 && p < (int)sizeof rs - 24; st++)
            for (int k = 0; k < 32 && p < (int)sizeof rs - 24; k++)
                if (g_samp_fill[st][k]) p += snprintf(rs + p, sizeof rs - p, " %d:%d=%llu", st, k, (unsigned long long)atomic_load(&g_samp_fill[st][k]));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 采样器:登记%llu 用%llu 查无%llu 建败%llu",
                     (unsigned long long)atomic_load(&g_samp_reg), (unsigned long long)atomic_load(&g_samp_use),
                     (unsigned long long)atomic_load(&g_samp_none), (unsigned long long)atomic_load(&g_samp_bad));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 哑因:没图%llu 空图%llu 深度非影%llu 影子给颜色%llu 强退哑图%llu｜影子真深度%llu 真绑但没传过%llu｜采空时兄弟有货%llu 谁都没货%llu",
                     (unsigned long long)atomic_load(&g_b_noimg), (unsigned long long)atomic_load(&g_b_empty),
                     (unsigned long long)atomic_load(&g_b_depth_plain),
                     (unsigned long long)atomic_load(&g_b_sh_color), (unsigned long long)atomic_load(&g_b_forced),
                     (unsigned long long)atomic_load(&g_b_shadow), (unsigned long long)atomic_load(&g_b_neverup),
                     (unsigned long long)atomic_load(&g_dy_sib), (unsigned long long)atomic_load(&g_dy_nosib));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 按需新建%llu 张（上限%d，撞顶%llu 次）｜贴图表%llu/%d",
                     (unsigned long long)atomic_load(&g_tex_make), (g_skip & 131072) ? TEX_MAKE_MAX_HI : TEX_MAKE_MAX,
                     (unsigned long long)atomic_load(&g_tex_make_full), (unsigned long long)g_tex_n, VTN);
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 上传像素有内容%llu 张、全零%llu 张",
                     (unsigned long long)atomic_load(&g_up_content), (unsigned long long)atomic_load(&g_up_zero));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 同 serial 二次建图 %llu 次（=画在这张、采在那张）", (unsigned long long)g_tex_dupser);
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 按池偏移复用%llu 张｜回收站复活%llu 张（进%llu 淘%llu 站%d/%d %lluMB）｜漏判同池偏移%llu｜新生命待重传%llu",
                     (unsigned long long)g_tex_stor, (unsigned long long)g_tex_revive,
                     (unsigned long long)g_recy_put, (unsigned long long)g_recy_lost,
                     g_recy_n, RECY_N, (unsigned long long)(g_recy_bytes >> 20),
                     (unsigned long long)g_tex_stormis, (unsigned long long)g_tex_stale);
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 贴图槽候选命中 bank%llu binding%llu reg%llu 全空%llu",
                     (unsigned long long)atomic_load(&g_cand_bank), (unsigned long long)atomic_load(&g_cand_bind),
                     (unsigned long long)atomic_load(&g_cand_reg), (unsigned long long)atomic_load(&g_cand_none));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 着色要空槽 槽=次数");
        for (int k = 0; k < 32 && p < (int)sizeof rs - 20; k++)
            if (g_bind_miss[k]) p += snprintf(rs + p, sizeof rs - p, " %d=%llu", k, (unsigned long long)atomic_load(&g_bind_miss[k]));
        GVL("%{public}s", rs);
        p = snprintf(rs, sizeof rs, "[vk] 真绑 阶段=次数");
        for (int st = 0; st < 6 && p < (int)sizeof rs - 20; st++)
            if (g_bind_real_st[st]) p += snprintf(rs + p, sizeof rs - p, " %d=%llu", st, (unsigned long long)atomic_load(&g_bind_real_st[st]));
        GVL("%{public}s", rs);
    }
}

// ---------------- M2-3b：重放游戏自己的绘制 | replaying the game's own draws ----------------
// 输入：构建机折出的 shaders.bin（键 → SPIR-V + 绑定表 + 顶点输入 + c1），真机报出的段键（M2-3a）。
// 流程：状态镜像 S（照 nvn_mtl.m:2275 的 S）→ 每次提交把池里的顶点 / 常量数据搬进一块 host 可见大缓冲
//       （Maleoon 没有 VK_EXT_external_memory_host，第 34 轮实测）→ 管线按（程序对、通道、拓扑、混合 / 剔除 / 深度、顶点布局）缓存
//       → 先只画索引三角形（第 66 轮实测占 94%）。
// Inputs: shaders.bin from the build box (key -> SPIR-V + binding table + vertex inputs + c1) and the stage keys measured on device (M2-3a).
// Flow: a state mirror S (mirroring nvn_mtl.m:2275) -> per submission the pool's vertex / constant data is copied into one host-visible
// arena (Maleoon has no VK_EXT_external_memory_host, measured round 34) -> pipelines cached by (program pair, pass, topology,
// blend / cull / depth, vertex layout) -> indexed triangles first (94% of draws per round 66).
static uint32_t g_lim_ubo = 65536, g_lim_ssbo = 0x40000000u;   // vk_boot 之后由实测上限夹住 | clamped by the measured limits once the device is up
static float g_aniso_max = 1.0f;                 // 各向异性上限（实测 maxSamplerAnisotropy + 特性开关）
static uint32_t g_max1d = 0;                     // 一维图宽度上限：缓冲维查表贴图要按它决定能不能建 1D 视图
// 压缩格式能力：马良是移动 GPU，很可能只有 ASTC 没有 BC；收不收只能问它自己
// Which compressed formats the GPU samples: a mobile GPU may have ASTC but no BC, so this has to be asked, not guessed.
static void probe_bc(void)
{
    static const struct { uint32_t bit; VkFormat f; const char *n; } t[] = {
        { 1, VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1" }, { 2, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1A" },
        { 4, VK_FORMAT_BC2_UNORM_BLOCK, "BC2" }, { 8, VK_FORMAT_BC3_UNORM_BLOCK, "BC3" },
        { 16, VK_FORMAT_BC4_UNORM_BLOCK, "BC4" }, { 32, VK_FORMAT_BC5_UNORM_BLOCK, "BC5" },
        { 64, VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H" }, { 128, VK_FORMAT_BC7_UNORM_BLOCK, "BC7" },
        { 256, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC4" } };
    uint32_t got = 0;
    char line[128]; int q = snprintf(line, sizeof line, "[vk] 压缩格式：");
    for (int i = 0; i < 9; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_pd, t[i].f, &fp);
        int ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        if (ok) got |= t[i].bit;
        if (q < (int)sizeof line - 12) q += snprintf(line + q, sizeof line - q, " %s%d", t[i].n, ok);
    }
    g_bcap = got;
    GVL("%{public}s", line);
}

static void vk_read_limits(void)
{
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_pd, &p);
    g_lim_ubo = p.limits.maxUniformBufferRange;
    g_lim_ssbo = p.limits.maxStorageBufferRange;
    g_aniso_max = p.limits.maxSamplerAnisotropy;
    VkPhysicalDeviceFeatures f;   // 这台设备报不支持就按各向 1 处理：开着 enable 建采样器会直接失败
    vkGetPhysicalDeviceFeatures(g_pd, &f);
    if (!f.samplerAnisotropy) g_aniso_max = 1.0f;
    g_max1d = p.limits.maxImageDimension1D;
    GVL("[vk] 上限：uboRange %{public}u ssboRange %{public}u uboAlign %{public}u 描述符组 %{public}u 顶点绑定 %{public}u｜图幅 1D %{public}u 2D %{public}u 各向异性 %{public}.1f 采样 lod 偏 %{public}.1f",
        g_lim_ubo, g_lim_ssbo, (unsigned)p.limits.minUniformBufferOffsetAlignment,
        p.limits.maxBoundDescriptorSets, p.limits.maxVertexInputBindings,
        g_max1d, p.limits.maxImageDimension2D, (double)p.limits.maxSamplerAnisotropy, (double)p.limits.maxSamplerLodBias);
}

// ---- 着色器二进制库 | the binary shader library ----
#define SHB_MAXE 512
static uint8_t *g_shb_map;
static const shb_hdr *g_shb_h;
static const shb_entry *g_shb_e; static const shb_bind *g_shb_b; static const shb_vi *g_shb_vi;
static const uint8_t *g_shb_c1; static const uint32_t *g_shb_spv;
static int g_shb_n;
static int g_shb_idx[1024];         // 段键 → 条目号 + 1（0 = 空）| stage key -> entry index + 1 (0 = empty)
static int g_shb_ready, g_shb_fail;

static uint64_t k8v(const uint8_t *k) { uint64_t v; memcpy(&v, k, 8); return v; }
// 十六进制键要折成和 k8v 一样的字节序：k8v 是 memcpy 8 个原字节（小端 → 第 0 字节落在低位），
// 按"先读的高位"拼出来的数与它正好反 8 字节，查表永远miss（第 68 轮实测 无库 = 全部绘制）。
// The hex key must fold into the same byte order as k8v, which memcpy()s 8 raw bytes (little-endian: byte 0 lands in the low bits).
// Building the value most-nibble-first is byte-reversed, so every lookup misses - round 68 measured 无库 on 100% of draws.
static uint64_t hexk(const char *h) { uint64_t v = 0; for (int i = 0; i < 8; i++) { char a = h[2 * i], b = h[2 * i + 1];
    if (!a || !b) break; v |= (uint64_t)((a <= '9' ? a - '0' : a - 'a' + 10) << 4 | (b <= '9' ? b - '0' : b - 'a' + 10)) << (8 * i); } return v; }
static unsigned shb_slot(uint64_t k) { return (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 52) & 1023u; }

static int shb_get(uint64_t k, int stage)
{
    unsigned s = shb_slot(k);
    for (int t = 0; t < 32; t++) {
        int v = g_shb_idx[(s + t) & 1023u];
        if (!v) return -1;
        const shb_entry *e = &g_shb_e[v - 1];
        if (k8v(e->key) == k && e->stage == stage) return v - 1;
    }
    return -1;
}

static int shb_has(uint64_t k)
{
    unsigned s = shb_slot(k);
    for (int t = 0; t < 32; t++) { int v = g_shb_idx[(s + t) & 1023u]; if (!v) return 0; if (k8v(g_shb_e[v - 1].key) == k) return 1; }
    return 0;
}

static int shb_load(void);   // 定义在下面 | defined below

// 顶/像段键对 → 库条目号；返回 0 = 库还没就绪，下次绑定再解（不把它当成"库里没有"）
// Vertex + fragment stage keys -> library entry indices; returning 0 means the library is not ready yet - retry on a later bind rather than treat it as "absent".
static int shb_pair(const char *k0, const char *k1, int *vi, int *fi)
{
    if (!g_shb_ready && !g_shb_fail && !shb_load()) { if (!g_shb_fail) return 0; }
    *vi = k0 && k0[0] ? shb_get(hexk(k0), 0) : -1;
    *fi = k1 && k1[0] ? shb_get(hexk(k1), 1) : -1;
    return 1;
}

// 一个键自己去库里认：返回 1 = 库已就绪，vi / fi 里哪一个成立就是那一段
// One key asks the library who it is: returning 1 means the library is ready, and whichever of vi / fi is >= 0 names that stage
static int shb_stage(const char *k, int *vi, int *fi)
{
    *vi = *fi = -1;
    if (!k || !k[0]) return 0;
    if (!g_shb_ready && !g_shb_fail && !shb_load()) return 0;
    uint64_t h = hexk(k);
    *vi = shb_get(h, 0);
    *fi = shb_get(h, 1);
    return 1;
}

static int shb_load(void)
{
    if (g_shb_ready) return 1;
    if (g_shb_fail) return 0;
    const char *over = getenv("DM_SHBIN");   // 显式指一条库文件（沙箱里 push 进去的那份）：换库不用重打 2.2 GB 的包
    const char *res = getenv("DM_RES");
    char path[512];
    if (over && over[0]) snprintf(path, sizeof path, "%s", over);
    else if (!res || !res[0]) { GVL("[vk] 没有 DM_RES：拿不到 shaders.bin，绘制不重放"); g_shb_fail = 1; return 0; }
    else snprintf(path, sizeof path, "%s/shaders.bin", res);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { GVL("[vk] 打不开 %{public}s：绘制不重放", path); g_shb_fail = 1; return 0; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof(shb_hdr)) { close(fd); g_shb_fail = 1; return 0; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { GRED("mmap shaders.bin 失败（%{public}lld 字节）", (long long)st.st_size); g_shb_fail = 1; return 0; }
    const shb_hdr *h = (const shb_hdr *)m;
    // spv_total / c1_total 是字节数（构建机 shbin_gen.py 记的就是长度），nvis 是条数、每条 4 字节
    // spv_total / c1_total are byte counts (that is what shbin_gen.py writes); nvis is an entry count at 4 bytes each
    size_t need = sizeof *h + (size_t)h->nentries * sizeof(shb_entry) + (size_t)h->nbinds * sizeof(shb_bind)
                + (size_t)h->nvis * 4 + (size_t)h->c1_total + (size_t)h->spv_total;
    if (h->magic != SHB_MAGIC || h->ver != SHB_VER || h->nentries > SHB_MAXE || need > (size_t)st.st_size) {
        GRED("着色器库不符：magic %{public}#x 版 %{public}u 条目 %{public}u 需要 %{public}zu 文件 %{public}lld",
             h->magic, h->ver, h->nentries, need, (long long)st.st_size);
        munmap(m, (size_t)st.st_size); g_shb_fail = 1; return 0;
    }
    g_shb_map = (uint8_t *)m; g_shb_h = h;
    g_shb_e = (const shb_entry *)(h + 1);
    g_shb_b = (const shb_bind *)(g_shb_e + h->nentries);
    g_shb_vi = (const shb_vi *)(g_shb_b + h->nbinds);
    g_shb_c1 = (const uint8_t *)(g_shb_vi + h->nvis);
    g_shb_spv = (const uint32_t *)(g_shb_c1 + h->c1_total);
    g_shb_n = (int)h->nentries;
    for (int i = 0; i < g_shb_n; i++) {
        const shb_entry *e = &g_shb_e[i];
        if (e->bind_off + e->nb > h->nbinds || e->vi_off + e->nvi > h->nvis
            || e->spv_off + e->spv_words > h->spv_total / 4 || e->c1_off + e->c1_len > h->c1_total) {
            GRED("着色器库第 %{public}d 条越界：绑定 %{public}u+%{public}u 顶 %{public}u+%{public}u spv %{public}u+%{public}u/%{public}u c1 %{public}u+%{public}u/%{public}u",
                 i, e->bind_off, e->nb, e->vi_off, e->nvi, e->spv_off, e->spv_words, h->spv_total / 4, e->c1_off, e->c1_len, h->c1_total);
            munmap(m, (size_t)st.st_size); g_shb_fail = 1; g_shb_n = 0; return 0;
        }
        unsigned s = shb_slot(k8v(e->key));
        for (int t = 0; t < 32; t++) { unsigned q = (s + t) & 1023u; if (!g_shb_idx[q]) { g_shb_idx[q] = i + 1; break; } }
    }
    g_shb_ready = 1;
    GVL("[vk] 着色器库就位：%{public}d 条、绑定 %{public}u、顶点输入 %{public}u、c1 %{public}u 字、SPIR-V %{public}u 字（%{public}s）",
        g_shb_n, h->nbinds, h->nvis, h->c1_total, h->spv_total / 4, path);
    return 1;
}

// ---- 每段一个 VkShaderModule / 描述符布局 / c1 缓冲（懒建）| per stage: module, descriptor layout, c1 buffer (lazy) ----
typedef struct {
    VkShaderModule mod; VkDescriptorSetLayout dsl;
    VkBuffer c1b; VkDeviceMemory c1m; uint8_t *c1map; uint64_t c1sz;
    uint8_t bad, tried;                       // bad = 有可写图像 / 缓冲贴图，这刀不画 | a storage image or texel buffer: this cut skips those draws
    uint8_t scop;                             // 段里有 subgroup / 屏障 / 原子指令（Op 224-225、227-242、318-319、333-366）| subgroup, barrier or atomic instructions present
    uint8_t feat;                             // 这段 SPIR-V 里做过的改写（见 FF_*），失败清单要用它做相关性 | rewrites applied to this module, used by the failure list
} shobj;
enum { FF_OLL = 1, FF_BB = 2, FF_EXT = 4, FF_CAP = 8 };   // OriginLowerLeft 翻向上 / BufferBlock 改 SSBO / 删扩展声明 / 删能力声明
static _Atomic uint64_t g_sho_ok, g_mod_fail, g_cap_drop, g_ext_drop, g_oll_fix, g_bb_fix, g_bufdim_fix;   // 建成的着色模块数 / vkCreateShaderModule 失败数 / 删掉的能力声明条数 | modules built, module failures, capabilities stripped
#define SH_OBJ_N SHB_MAXE
static shobj g_sho[SH_OBJ_N];

static VkShaderStageFlagBits sh_stage(int s)
{
    return s == 0 ? VK_SHADER_STAGE_VERTEX_BIT : s == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
}
static VkDescriptorType desc_type(int k)
{
    return k == SHB_UBO ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
         : k == SHB_SSBO ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
         : k == SHB_IMAGE ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static int mk_hostbuf(uint64_t size, uint32_t usage, VkBuffer *b, VkDeviceMemory *m, void **map)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_dev, &bi, NULL, b) != VK_SUCCESS) { GRED("建缓冲 %{public}llu 字节失败", (unsigned long long)size); return 0; }
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, *b, &rq);
    int ti = find_type(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size; ai.memoryTypeIndex = ti >= 0 ? (uint32_t)ti : g_mem_host_visible;
    if (vkAllocateMemory(g_dev, &ai, NULL, m) != VK_SUCCESS) { GRED("建缓冲内存 %{public}llu 失败", (unsigned long long)rq.size); vkDestroyBuffer(g_dev, *b, NULL); *b = NULL; return 0; }
    if (vkBindBufferMemory(g_dev, *b, *m, 0) != VK_SUCCESS) { GRED("绑缓冲内存失败"); vkDestroyBuffer(g_dev, *b, NULL); *b = NULL; vkFreeMemory(g_dev, *m, NULL); *m = NULL; return 0; }
    void *p = NULL;
    if (vkMapMemory(g_dev, *m, 0, rq.size, 0, &p) != VK_SUCCESS) { GRED("映射缓冲失败"); vkDestroyBuffer(g_dev, *b, NULL); *b = NULL; vkFreeMemory(g_dev, *m, NULL); *m = NULL; return 0; }
    if (map) *map = p;
    return 1;
}

// SPIR-V 能力删除表：马良 920 的着色编译器对某些能力直接 SPV01 拒编（第 69 轮实测 21689 次
// "Unsupported capability (10)"，管线 0 条）。离线普查 98 段（用 dxvk 自带的 spirv.hpp 反查枚举名）：
// 声明恒为 {1 Shader, 10 Float64, 25 ImageGatherExtended, 46 SampledBuffer, 47 ImageBuffer, 50 ImageQuery,
// 62/64/65 GroupNonUniform Vote/Ballot/Shuffle, 4427 DrawParameters, 5378 FragmentShaderPixelInterlockEXT}。
// 删掉整条 OpCapability（定长 2 字，删后指令流仍合法）不丢功能，所以这里做成"按驱动点名的黑名单"，
// 默认删 10 / 4427 / 5378 —— 离线普查证明这三条在整个指令库里都找不到对应用：全库类型只有 32 位浮点，
// 没有 5384/5385 Begin/EndInvocationInterlock，也没有 4424 BaseVertex / 4425 BaseInstance / 4426 DrawIndex
// （顶点段用到的内置量只有 42 VertexIndex，那是核心自带的，不需要 DrawParameters）。
// 驱动再点名哪一个，就把编号加进 DM_VK_STRIP_CAPS 重启一次即可，不用重编。
// Capability strip list: the Maleoon 920 compiler rejects some capabilities outright with SPV01 (round 69: 21689
// "Unsupported capability (10)", zero pipelines). Every module declares the same fixed block {1 Shader, 10 Float64,
// 25 ImageGatherExtended, 46 SampledBuffer, 47 ImageBuffer, 50 ImageQuery, 62/64/65 GroupNonUniform, 4427 DrawParameters,
// 5378 FragmentShaderPixelInterlockEXT}. OpCapability is two fixed words, so deleting the whole instruction keeps the
// stream legal, hence this is a blacklist the driver names one at a time. The default - 10, 4427, 5378 - is provably
// free: the only types anywhere are 32-bit, there is no 5384/5385 Begin/EndInvocationInterlock, and no 4424 BaseVertex /
// 4425 BaseInstance / 4426 DrawIndex (vertex modules touch builtin 42 VertexIndex, which is core and needs no capability).
// A newly named id just goes into DM_VK_STRIP_CAPS on the next launch, no rebuild.
static int cap_strip(uint32_t id, int *dropped)
{
    static int inited;
    static uint32_t list[16];
    static int n;
    if (!inited) {
        const char *s = getenv("DM_VK_STRIP_CAPS");
        if (!s || !*s) s = "10,4427,5378";
        for (const char *q = s; *q && n < 16; ) {
            char *end; unsigned long v = strtoul(q, &end, 10);
            if (end == q) { q++; continue; }
            list[n++] = (uint32_t)v; q = end;
        }
        GVL("[vk] SPIR-V 能力删除表 = %{public}s（%{public}d 条）", s, n);
        inited = 1;
    }
    for (int i = 0; i < n; i++) if (list[i] == id) { (*dropped)++; return 1; }
    return 0;
}

// 扩展名删除表：删了能力声明还得删配套的 OpExtension，否则严格的前端会因为一个"没人用的未知扩展名"再拒一次。
// 目前只有一条：SPV_EXT_fragment_shader_interlock（59 个像素段全带，全库没有一个 interlock 指令）。
// Extension-name blacklist: dropping a capability must also drop its OpExtension, or a strict front end can reject the
// unknown-but-unused extension name. Only one today: SPV_EXT_fragment_shader_interlock (all 59 fragment modules carry it,
// with zero interlock instructions anywhere).
static int ext_strip(const uint32_t *lit, uint32_t nwords)
{
    static int inited;
    static char names[4][64];
    static int n;
    if (!inited) {
        const char *s = getenv("DM_VK_STRIP_EXT");
        if (!s || !*s) s = "SPV_EXT_fragment_shader_interlock";
        const char *q = s;
        while (*q && n < 4) {
            const char *r = q;
            while (*r && *r != ',') r++;
            size_t L = (size_t)(r - q);
            if (L && L < sizeof names[0]) { memcpy(names[n], q, L); names[n][L] = 0; n++; }
            q = (*r == ',') ? r + 1 : r;
        }
        GVL("[vk] SPIR-V 扩展删除表 = %{public}s（%{public}d 条）", s, n);
        inited = 1;
    }
    char buf[64];
    size_t u = 0;
    for (uint32_t i = 0; i < nwords && u + 1 < sizeof buf; i++)
        for (int k = 0; k < 4; k++) {
            char c = (char)((lit[i] >> (8 * k)) & 0xFFu);
            if (!c) { buf[u] = 0; u++; goto done; }
            buf[u++] = c;
        }
done:
    if (!u) return 0;
    buf[sizeof buf - 1] = 0;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], buf)) return 1;
    return 0;
}

static int sho_prep(int ei)
{
    shobj *o = &g_sho[ei];
    if (o->tried) return o->mod && o->dsl;
    o->tried = 1;
    const shb_entry *e = &g_shb_e[ei];
    // 只挡可写图像：缓冲维的采样绑定（离线普查：22 条像素段各一条，全是只读查表）不再挡——
    // 它的 SPIR-V 在下面被改成 1D（Fetch 的坐标本来就是单个 i32，1D 正好），宿主绑 1D 视图。
    // Only writable images stay blocked. The buffer-dim read-only lookups (the offline census: one per blocked fragment entry, 22 in all)
    // are unblocked - the rewriter below turns them into 1D, whose Fetch coordinate is a single i32 exactly like the buffer's, and the
    // host binds a 1D view.
    for (int i = 0; i < e->nb; i++) {
        const shb_bind *b = &g_shb_b[e->bind_off + i];
        if (b->kind == SHB_IMAGE) { o->bad = 1; break; }
    }
    if (o->bad) { static int said; if (!said++) GVL("[vk] 条目 %{public}d 有可写图像：这刀跳过它的绘制", ei); return 0; }
    if (e->nb > 64) { GRED("条目 %{public}d 绑定 %{public}d 条，超 64 上限", ei, e->nb); return 0; }
    // 复制一份再改：库是只读映射。SPIR-V 里每条 DescriptorSet 都改成"本阶段自己那一组"（顶点 0、其余 1）：
    // 离线反射证明每个资源都带显式 set 装饰（第 67 轮实测 Binding/DescriptorSet 各 1073 条），而 binding 是按阶段从 0 连号的，
    // 不改号顶点和像素会撞在同一组里。
    // Copy first - the library is mapped read-only. Every DescriptorSet decoration becomes this stage's own set (vertex 0, everything else 1):
    // the offline reflection proved all resources carry an explicit set (round 67: Binding 1073 / DescriptorSet 1073), and bindings are numbered
    // per stage from 0, so without the rewrite the vertex and fragment halves would collide inside one set.
    uint32_t n = e->spv_words;
    uint32_t *w = (uint32_t *)malloc((size_t)n * 4);
    if (!w || n < 6) { if (w) free(w); GRED("条目 %{public}d：%{public}u 字 SPIR-V 装不下", ei, n); return 0; }
    memcpy(w, g_shb_spv + e->spv_off, (size_t)n * 4);
    // 三遍：① 找出带 BufferBlock 装饰的结构体 id；② 找出指向它们的 Uniform 指针 id；③ 边压缩边改。
    // BufferBlock 是 SPIR-V 1.3 里废弃的 OpenGL 写法：块在 Uniform 存储类里 = 联合缓冲区。全库 43 个块都是这个形状，
    // 且成员偏移（DecorationOffset）都齐（离线普查逐块核对），所以改成 Vulkan 的 SSBO 写法是纯机械的：
    // 装饰 BufferBlock(3) -> Block(2)、指针与变量的存储类 Uniform(2) -> StorageBuffer(12)，宿主侧早就按 STORAGE_BUFFER 绑了。
    // 同一遍里还顺手把 OriginLowerLeft(7) 翻成 OriginUpperLeft(6) —— 59 个像素段全带前者，而 Vulkan 只认后者。
    // Three passes: collect the BufferBlock-decorated struct ids, then the Uniform pointers aimed at them, then rewrite while compacting.
    // BufferBlock is the deprecated OpenGL-era spelling: a block in the Uniform storage class. All 43 blocks in the library have
    // that shape and every member already carries an explicit Offset (checked block by block offline), so the conversion to the
    // Vulkan SSBO form is purely mechanical: decoration BufferBlock(3) -> Block(2), pointer / variable storage class
    // Uniform(2) -> StorageBuffer(12) - and the host side has bound them as STORAGE_BUFFER all along. The same pass also flips
    // OriginLowerLeft(7) to OriginUpperLeft(6), which all 59 fragment modules need because Vulkan only accepts the latter.
    uint32_t bound = w[3] + 1u;
    uint8_t *bb = (uint8_t *)calloc(bound, 1), *pp = (uint8_t *)calloc(bound, 1);
    uint32_t i = 5, out = 5; int ndrop = 0, nbb = 0;
    if (!bb || !pp) { free(bb); free(pp); GRED("条目 %{public}d：%{public}u 个 id 的标记表装不下", ei, bound); return 0; }
    while (i + 1 < n) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!wc || wc > n - i) break;
        if (op == 71u && wc >= 3 && w[i + 2] == 3u && w[i + 1] < bound && !bb[w[i + 1]]) { bb[w[i + 1]] = 1; nbb++; }   // OpDecorate BufferBlock
        i += wc;
    }
    i = 5;
    while (i + 1 < n) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!wc || wc > n - i) break;
        if (op == 32u && wc >= 4 && w[i + 2] == 2u && w[i + 3] < bound && bb[w[i + 3]] && w[i + 1] < bound) pp[w[i + 1]] = 1;   // OpTypePointer Uniform -> 那个块
        i += wc;
    }
    i = 5;
    while (i + 1 < n) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!wc || wc > n - i) break;
        if (op == 17u && wc >= 2 && cap_strip(w[i + 1], &ndrop)) { o->feat |= FF_CAP; i += wc; continue; }   // OpCapability
        if (op == 10u && wc >= 2 && ext_strip(w + i + 1, wc - 1)) { o->feat |= FF_EXT; atomic_fetch_add(&g_ext_drop, 1); i += wc; continue; }   // OpExtension
        if ((op >= 224u && op <= 225u) || (op >= 227u && op <= 242u) || op == 318u || op == 319u
            || (op >= 333u && op <= 366u)) o->scop = 1;                                       // 屏障 / 原子 / 组内指令 | barrier, atomic or group ops
        if (op == 16u && wc >= 3 && w[i + 2] == 7u) { w[i + 2] = 6u; o->feat |= FF_OLL; atomic_fetch_add(&g_oll_fix, 1); }   // OpExecutionMode OriginLowerLeft -> UpperLeft
        // OpTypeImage 的 Dim：缓冲(5) -> 一维(0）。字序 1=结果 2=采样类型 3=Dim；Dim=Buffer 在 Vulkan 要 VkBufferView 那条路，
        // 而离线普查证明这 22 条全用它做只读查表，Fetch 的坐标本来就是单个 i32 —— 1D 与它同形，改完宿主绑 1D 视图即可。
        // OpTypeImage Dim Buffer(5) -> 1D(0). Word order is 1=result 2=sampled type 3=dim. Dim=Buffer would need the VkBufferView route,
        // but the census shows all 22 uses are read-only lookups whose OpImageFetch coordinate is already a single i32 - 1D is the same
        // shape, so the rewrite is drop-in and the host binds a 1D view.
        if (op == 25u && wc >= 9u && w[i + 3] == 5u) { w[i + 3] = 0u; atomic_fetch_add(&g_bufdim_fix, 1); }
        if (op == 71u && wc >= 3) {
            if (w[i + 2] == 34u && wc >= 4) w[i + 3] = e->stage == 0 ? 0u : 1u;             // OpDecorate / DecorationDescriptorSet（带操作数，4 字起）
            else if (w[i + 2] == 3u && w[i + 1] < bound && bb[w[i + 1]]) { w[i + 2] = 2u; o->feat |= FF_BB; }   // BufferBlock -> Block（无操作数，正好 3 字）
        }
        if (op == 32u && wc >= 4 && w[i + 1] < bound && pp[w[i + 1]]) w[i + 2] = 12u;         // OpTypePointer Uniform -> StorageBuffer
        if (op == 59u && wc >= 4 && w[i + 1] < bound && pp[w[i + 1]] && w[i + 3] == 2u) w[i + 3] = 12u;   // OpVariable Uniform -> StorageBuffer
        if (out != i) memmove(w + out, w + i, (size_t)wc * 4);
        out += wc; i += wc;
    }
    if (i < n) { memmove(w + out, w + i, (size_t)(n - i) * 4); out += n - i; }   // 读不满一条指令的尾巴原样带走，长度按搬走的字算 | unreadable tail kept verbatim, counted by words actually moved
    if (ndrop) atomic_fetch_add(&g_cap_drop, (uint64_t)ndrop);
    if (nbb) { o->feat |= FF_BB; atomic_fetch_add(&g_bb_fix, 1); }
    free(bb); free(pp);
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = (size_t)out * 4; mi.pCode = w;
    VkResult r = vkCreateShaderModule(g_dev, &mi, NULL, &o->mod);
    free(w);
    if (r != VK_SUCCESS) { atomic_fetch_add(&g_mod_fail, 1); GVL("[red] 建游戏着色模块（条目 %{public}d 段 %{public}d，%{public}u 字）= %{public}d (%{public}s)", ei, e->stage, (unsigned)mi.codeSize / 4, (int)r, vr(r)); return 0; }
    VkDescriptorSetLayoutBinding lb[64]; uint32_t cnt = 0;
    for (int i = 0; i < e->nb; i++) {
        const shb_bind *b = &g_shb_b[e->bind_off + i];
        lb[cnt++] = (VkDescriptorSetLayoutBinding){ b->binding, desc_type(b->kind), 1, sh_stage(e->stage), NULL };
    }
    VkDescriptorSetLayoutCreateInfo di = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    di.bindingCount = cnt; di.pBindings = lb;
    if (vkCreateDescriptorSetLayout(g_dev, &di, NULL, &o->dsl) != VK_SUCCESS) { GRED("建游戏描述符布局失败（条目 %{public}d，%{public}u 条）", ei, cnt); vkDestroyShaderModule(g_dev, o->mod, NULL); o->mod = NULL; return 0; }
    if (e->c1_len) {
        o->c1sz = e->c1_len > 8192 ? e->c1_len : 8192;   // 声明块上限 64 KB，实测 c1 最长 512 字节 | declared blocks cap at 64 KB; measured c1 max 512 bytes
        if (!mk_hostbuf(o->c1sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &o->c1b, &o->c1m, (void **)&o->c1map)) return 0;
        memset(o->c1map, 0, (size_t)o->c1sz);
        memcpy(o->c1map, g_shb_c1 + e->c1_off, e->c1_len);
    }
    if (o->mod && o->dsl) atomic_fetch_add(&g_sho_ok, 1);
    return o->mod && o->dsl;
}

// ---- NVN 状态镜像（照 nvn_mtl.m:2275 的 S）| the NVN state mirror ----
static struct {
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][128], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp;
    float blend_color[4];
    p_scissor sc; int has_sc;
    int64_t copy_stride;      // OP_COPY_STRIDE：下一条 B2T 的行距（0 = 按宽度自己算）| row pitch for the next buffer->texture copy
} S;
static int g_args_dirty[6];       // 该阶段的绑定表变过 ⇒ 描述符集要重编（与参照实现的 g_args_dirty 同义）| a stage's binding table changed: rebuild its descriptor set

static void args_dirty_all(void) { for (int i = 0; i < 6; i++) g_args_dirty[i] = 1; }

static void state_reset(void)
{
    memset(&S, 0, sizeof S);
    S.chmask.mask = 0xFFFFFFFFu;
    S.poly = (poly_st){ NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0 };
    S.ds = (ds_st){ 0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1 };
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xFF;
    S.dr[1] = 1.0f;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){ i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD };
    args_dirty_all();
}

static int state_op(uint32_t op, const void *a)
{
    switch (op) {
    case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
    case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
    case OP_BIND_PROGRAM: { const prog_t *np = ((const p_bind_program *)a)->prog; if (np != S.prog) { S.prog = np; args_dirty_all(); } break; }
    case OP_BIND_STATE: {
        const p_bind_state *b = (const p_bind_state *)a; void *dst = NULL; size_t n = 0;
        blend_st bs;
        switch (b->kind) {
        case 0: memcpy(&bs, b->bytes, sizeof bs); dst = &S.blend[bs.target & 7]; n = sizeof bs; break;
        case 1: dst = &S.color; n = sizeof S.color; break;
        case 2: dst = &S.chmask; n = sizeof S.chmask; break;
        case 3: dst = &S.ms; n = sizeof S.ms; break;
        case 4: dst = &S.poly; n = sizeof S.poly; break;
        case 5: dst = &S.ds; n = sizeof S.ds; break;
        }
        if (dst && memcmp(dst, b->bytes, n)) memcpy(dst, b->bytes, n);
        break; }
    case OP_BIND_VATTR: { const p_bind_vattr *b = (const p_bind_vattr *)a;
        if (b->n > 0 && b->n <= 16 && memcmp(S.attr, b->st, 4 * b->n)) { memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; } break; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const p_bind_vstream *)a;
        if (b->n > 0 && b->n <= 16 && memcmp(S.stream, b->st, 8 * b->n)) { memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; } break; }
    case OP_BIND_VBUF: { const p_bind_vbuf *b = (const p_bind_vbuf *)a;
        if (b->first >= 0 && b->first + b->n <= 16 && b->n > 0) memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
    case OP_BIND_UBO: { const p_bind_buf *b = (const p_bind_buf *)a;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16 && memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16 && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) { S.ssbo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
    case OP_BIND_TEX: case OP_BIND_SAMP: { const p_bind_handles *b = (const p_bind_handles *)a;
        // 先验界再取表指针：stage 越界时 &S.tex[b->stage][0] 本身就是野指针算术（这层的铁律是不把宿主搞崩）
        // Bounds before the pointer: taking &S.tex[b->stage][0] on an out-of-range stage is already wild pointer arithmetic.
        if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128 && b->n > 0) {
            uint64_t *tab = op == OP_BIND_TEX ? &S.tex[b->stage][0] : &S.samp[b->stage][0];
            if (memcmp(tab + b->first, b->h, 8 * b->n)) {
                memcpy(tab + b->first, b->h, 8 * b->n); g_args_dirty[b->stage] = 1;
                if (op == OP_BIND_TEX) {
                    for (int k = 0; k < b->n; k++)
                        if (b->h[k] & 0xfffff) atomic_fetch_add(&g_slot_fill[b->stage][b->first + k < 32 ? b->first + k : 31], 1);
                // 采样器侧同样记一笔：合并绑定的 s 槽就填在这里，句柄 = 号<<20（低 20 位恒 0，见 hle_nvn.c:181）
                // The sampler side gets the same tally: the s slots of combined bindings land here, handle = id << 20 (low 20 bits always 0)
                } else {
                    for (int k = 0; k < b->n; k++)
                        if (b->h[k] >> 20) atomic_fetch_add(&g_samp_fill[b->stage][b->first + k < 32 ? b->first + k : 31], 1);
                }
            }
        }
        break; }
    case OP_BIND_IMG: { const p_bind_handles *b = (const p_bind_handles *)a;
        if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 32 && b->n > 0
            && memcmp(&S.img[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; }
        break; }
    case OP_STENCIL: { const p_stencil *b = (const p_stencil *)a;
        for (int f = 0; f < 2; f++) if (b->face & (1 << f)) {
            if (b->which == 0) S.st_vmask[f] = (uint32_t)b->value_mask;
            else if (b->which == 1) S.st_wmask[f] = (uint32_t)b->mask;
            else S.st_ref[f] = (uint32_t)b->ref;
        }
        break; }
    case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
    case OP_VIEWPORTS: { const p_viewports *b = (const p_viewports *)a;
        for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S.vp[b->first + i], b->v[i], 16);
        S.has_vp = 1; break; }
    case OP_DEPTH_RANGE: { const p_depth_range *b = (const p_depth_range *)a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
    case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
    default: return 0;
    }
    return 1;
}

// ---- host 可见大缓冲：池里的顶点 / 常量数据搬进来（没有 external_memory_host）| the host-visible arena ----
#define ARENA_SZ (64ull << 20)
#define ARENA_GUARD (1ull << 20)      // 尾巴留 1 MB 已清零的护区：着色器越界读落在自己地里，不打掉设备 | a zeroed tail so a stray over-read stays inside our own memory
static VkBuffer g_arena; static VkDeviceMemory g_arena_mem; static uint8_t *g_arena_map;
static uint64_t g_arena_off, g_arena_gen;
typedef struct { uint64_t addr, len, gen; uint32_t off; } adedup;
static adedup g_ahash[8192];         // 直映缓存：同一提交里同 (地址, 长度) 只搬一次 | direct-mapped: one copy per (address, length) inside a submission
static _Atomic uint64_t g_adup_hit, g_adup_new, g_adup_bad, g_arena_full;

static uint64_t arena_put(uint64_t addr, uint64_t len, uint64_t floor_sz, uint32_t *off)
{
    if (!addr) return 0;
    if (!len) len = 1;
    uint64_t need = (len > floor_sz ? len : floor_sz) & ~(uint64_t)15;
    need += 15;
    unsigned slot = (unsigned)(((addr ^ (len * 0x9E3779B1u)) * 0x9E3779B97F4A7C15ull) >> 51) & 8191u;
    adedup *h = &g_ahash[slot];
    if (h->gen == g_arena_gen && h->addr == addr && h->len == len) { *off = h->off; atomic_fetch_add(&g_adup_hit, 1); return 1; }
    void *base; uint64_t bo;
    if (!mtl_addr(addr, len, &base, &bo)) { atomic_fetch_add(&g_adup_bad, 1); return 0; }   // 不在任何池里：宁可不画 | not inside a pool: skip rather than dereference
    if (g_arena_off + need > ARENA_SZ - ARENA_GUARD) { atomic_fetch_add(&g_arena_full, 1); return 0; }
    uint64_t o = g_arena_off;
    g_arena_off = o + need;
    memcpy(g_arena_map + o, (uint8_t *)base + bo, len);
    if (need > len) memset(g_arena_map + o + len, 0, (size_t)(need - len));
    *h = (adedup){ addr, len, g_arena_gen, (uint32_t)o };
    *off = (uint32_t)o;
    atomic_fetch_add(&g_adup_new, 1);
    return 1;
}

// ---- 静态哑资源：全零缓冲、support、1x1 哑图与采样器 | the static dummies: zero buffer, support, 1x1 images and samplers ----
#define ZERO_SZ 65536
static VkBuffer g_zero; static VkDeviceMemory g_zero_mem;
static VkBuffer g_sup; static VkDeviceMemory g_sup_mem; static uint64_t g_sup_sz;
static VkImageView g_dim_view[5];   // 0 2D 1 2D阵列 2 3D 3 立方 4 一维（缓冲维查表：SPIR-V 已改成 1D，视图必须同族）
static VkImageView g_shview;        // shadow：一张深度 1x1 | the depth dummy a shadow sampler needs
static VkSampler g_smp_lin, g_smp_cmp;

static void mk_sampler(VkSampler *out, int cmp)
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;   // 哑图只有一级：mip 取最近点，避免 lod 选择越界 | one level, so nearest-mip keeps the lod in range
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0; si.maxLod = 0;
    si.compareEnable = cmp ? VK_TRUE : VK_FALSE;
    si.compareOp = cmp ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_NEVER;
    if (vkCreateSampler(g_dev, &si, NULL, out) != VK_SUCCESS) GRED("建哑采样器失败（%{public}d）", cmp);
}

// ---- 采样器登记：贴图按号查视图，采样器同理——游戏在采样器池里登记 samp_par，句柄 = 号 << 20（hle_nvn.c:181）。
// 第 87 轮之前这层是空函数，所有采样都用同一把「线性 + 钳边」哑采样器：游戏自己声明的最近点、镜像、mip 全丢了。
// 缓存按参数哈希去重、只建不销毁：一个号反复登记不同参数时，旧句柄还可能被已录的描述符引用，销毁就是踩已提交的命令。
// Sampler registration: textures resolve by ID and samplers work the same way - the game registers samp_par in the sampler pool,
// the handle being id << 20 (hle_nvn.c:181). Until round 87 this entry was a stub, so every sample used one linear/clamp dummy and
// the game's own nearest / mirror / mip choices were lost. The cache dedupes by parameter hash and never destroys: an old handle
// may still be referenced by recorded descriptor writes, so freeing it would touch submitted commands.
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
#define SREG_MAX 8192
typedef struct { uint32_t hash; int cmp; VkSampler s; } smpcache;
#define SMPC_N 512
static smpcache g_smpc[SMPC_N]; static int g_smpc_n;

static VkSamplerAddressMode vk_wrap(int w)
{
    switch (w) {
    case 0: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static uint32_t samp_hash(const samp_par *p)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof *p; i++) h = (h ^ b[i]) * 16777619u;
    return h ? h : 1;
}

static int samp_ok(int sid)
{
    if (sid <= 0) return 0;
    pthread_mutex_lock(&g_regm);
    int ok = sid < g_nsreg && g_sreg_ok && g_sreg_ok[sid];
    pthread_mutex_unlock(&g_regm);
    return ok;
}

// want_cmp：着色器把这张图当阴影图采（vtype bit4）。只有阴影路才开比较模式——
// 比较采样器配颜色视图是非法用法，而哑图那几档全是颜色。
// want_cmp is set when the shader samples this as a shadow map; only that route enables compare mode,
// because a compare sampler against a colour view is illegal usage and the dummy views are colour.
static VkSampler samp_get(int sid, int want_cmp)
{
    if (sid <= 0 || !g_on || !g_dev) return NULL;
    pthread_mutex_lock(&g_regm);
    if (sid >= g_nsreg || !g_sreg_ok[sid]) { pthread_mutex_unlock(&g_regm); atomic_fetch_add(&g_samp_none, 1); return NULL; }
    samp_par p = g_sreg[sid];
    uint32_t h = samp_hash(&p) ^ (uint32_t)(want_cmp * 0x9e3779b1u);
    for (int i = 0; i < g_smpc_n; i++) if (g_smpc[i].hash == h && g_smpc[i].cmp == want_cmp) {
        VkSampler s = g_smpc[i].s; pthread_mutex_unlock(&g_regm);
        if (s) atomic_fetch_add(&g_samp_use, 1);
        return s;
    }
    if (g_smpc_n >= SMPC_N) { pthread_mutex_unlock(&g_regm); return NULL; }
    pthread_mutex_unlock(&g_regm);

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = p.magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    // NVN 的 min 滤镜 0-5 = 最近 / 线性 / 最近-mip最近 / 线性-mip最近 / 最近-mip线性 / 线性-mip线性（nvn.h:885）
    si.minFilter = (p.minf == 1 || p.minf == 3 || p.minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = (p.minf == 4 || p.minf == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = vk_wrap(p.ws); si.addressModeV = vk_wrap(p.wt); si.addressModeW = vk_wrap(p.wr);
    // lod 夹取：NVN 缺省 -1000..1000，Vulkan 要求 minLod <= maxLod 且非负；上限收到 1000（驱动再按视图层数夹）
    si.minLod = p.lod_min > 0 ? p.lod_min : 0;
    si.maxLod = p.lod_max < si.minLod ? si.minLod : (p.lod_max > 1000 ? 1000 : p.lod_max);
    if (p.minf <= 1) { si.minLod = 0; si.maxLod = 0; }   // 不采 mip（Metal 那边的 NotMipmapped）
    if (p.lod_bias) si.mipLodBias = p.lod_bias;
    // 位 16777216 = 所有采样器夹到 0 级（禁 mip）：第 115 轮 B40 排除了像素与 mip 本身（级 1 与级 0 缩样相关 0.995），
    // 可屏幕上 UI 行是平涂、背景是一圈一圈的多边形带 —— 那正是「采到了最顶层 mip（全图平均色）」的样子。
    // 一枪分辨：夹到 0 级后美术若回来了，问题就在导数/UV 尺度（导数太大 ⇒ LOD 太高），不在贴图数据。
    // Bit 16777216 clamps every sampler to level 0. B40 proved the texels and the mip chain are correct, yet the screen shows
    // top-mip averages; if clamping brings the art back the fault is in the derivative/UV scale, not in the texture data.
    if (g_skip & 16777216) { si.minLod = 0; si.maxLod = 0; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; si.mipLodBias = 0; }
    // DM_MIPK=K：把所有采样器钉在第 K 级（不动滤波方式）。第 116 轮 B41 只证明「夹到 0 级画面就回来了」，
    // 没量出客体真正采到第几级；从 K=0 往上扫，画面从"平涂"跳回"有细节"的那一档就是它实际采到的级号，
    // 一次构建换 N 次免费重启（换 K 不用重装），比每档一轮真机便宜得多。
    {
        const char *mk = getenv("DM_MIPK");
        if (mk && mk[0] >= '0' && mk[0] <= '9') { int k = atoi(mk); si.minLod = (float)k; si.maxLod = (float)k; }
    }
    // DM_MAXLOD=N：只把 lod 上限收到 N，别的一律不动。这一枪是为了和 DM_MIPK 分家：
    // 两者都会让"画面突然变清楚"，但责任完全不同 —— 夹级号说明客体算出的 λ 真有那么大（导数坏），
    // 只收上限就说明 λ 一直很小、是驱动把 maxLod=1000（饥荒没设过，NVN 缺省就是这个）当成了级号去索引，
    // 越界回落到最小一级 = 全图平均色 = 屏幕上那片平涂。采样器在本层是按号缓存、跨贴图共用的，
    // 所以没法按"当前这张图有几级"去夹，只能试一个固定值。
    {
        const char *ml = getenv("DM_MAXLOD");
        if (ml && ml[0] >= '0' && ml[0] <= '9') {
            float n = (float)atoi(ml);
            if (si.maxLod > n) si.maxLod = n;
            if (si.minLod > si.maxLod) si.minLod = si.maxLod;
        }
    }
    if (p.aniso > 1.0f && g_aniso_max > 1.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = p.aniso > g_aniso_max ? g_aniso_max : p.aniso;
    }
    si.compareEnable = want_cmp ? VK_TRUE : VK_FALSE;
    si.compareOp = want_cmp ? (p.cmp_mode ? (VkCompareOp)(p.cmp_func - 1) : VK_COMPARE_OP_LESS_OR_EQUAL) : VK_COMPARE_OP_NEVER;
    if (si.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || si.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        si.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)
        si.borderColor = p.border[3] > 0.5f ? (p.border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK)
                                            : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = NULL;
    if (vkCreateSampler(g_dev, &si, NULL, &s) != VK_SUCCESS) {
        atomic_fetch_add(&g_samp_bad, 1);
        static int said; if (said < 4) { said++; GRED("建采样器失败 号%{public}d mag%{public}d min%{public}d 边%{public}d%{public}d 各向%{public}.1f", sid, p.magf, p.minf, p.ws, p.wt, p.aniso); }
        return NULL;
    }
    pthread_mutex_lock(&g_regm);
    int dup = 0;
    for (int i = 0; i < g_smpc_n; i++) if (g_smpc[i].hash == h && g_smpc[i].cmp == want_cmp) { dup = 1; break; }
    if (!dup && g_smpc_n < SMPC_N) g_smpc[g_smpc_n++] = (smpcache){ h, want_cmp, s };
    pthread_mutex_unlock(&g_regm);
    if (dup) { vkDestroySampler(g_dev, s, NULL); return NULL; }
    atomic_fetch_add(&g_samp_use, 1);
    static int saide;
    if (saide < 12) { saide++; GVL("[vk] 建采样器 号%{public}d mag%{public}d min%{public}d 边%{public}d%{public}d 比较%{public}d lod%{public}.1f..%{public}.1f 偏%{public}.2f 各向%{public}.1f 夹mip%{public}d 滤%{public}d",
                                  sid, p.magf, p.minf, p.ws, p.wt, want_cmp, si.minLod, si.maxLod, si.mipLodBias, p.aniso,
                                  (int)((g_skip & 16777216) != 0), (int)si.mipmapMode); }
    return s;
}

void mtl_reg_samp(int id, const samp_par *p)
{
    if (!p || id < 0 || id >= SREG_MAX) return;
    pthread_mutex_lock(&g_regm);
    if (id >= g_nsreg) {
        int want = id + 256; if (want > SREG_MAX) want = SREG_MAX;
        samp_par *np = (samp_par *)realloc(g_sreg, (size_t)want * sizeof *np);
        uint8_t *no = (uint8_t *)realloc(g_sreg_ok, (size_t)want);
        if (!np || !no) { pthread_mutex_unlock(&g_regm); return; }
        if (g_sreg_ok) memset(no + g_nsreg, 0, (size_t)(want - g_nsreg));
        g_sreg = np; g_sreg_ok = no; g_nsreg = want;
    }
    g_sreg[id] = *p; g_sreg_ok[id] = 1;
    atomic_fetch_add(&g_samp_reg, 1);
    pthread_mutex_unlock(&g_regm);
    // 偏=lod_bias 必须打：第 116 轮 B41 用「一律夹到 0 级」把美术整幅叫回来了（截图熵 +35~83%），
    // 说明锅在 LOD 选择而不是贴图数据，但夹 0 级一次动了 bias、导数、min/max 三样，分不出是哪一样。
    // 缺省 lod_bias=0（SetDefaults 先 memset），所以只要这里出现非零就是客体自己设的。
    static int said;
    if (said < 6) { said++; GVL("[vk] 登记采样器 号%{public}d mag%{public}d min%{public}d 边uvw=%{public}d%{public}d%{public}d 比较%{public}d%{public}d lod%{public}.1f..%{public}.1f 偏%{public}.2f 各向%{public}.1f",
                                id, p->magf, p->minf, p->ws, p->wt, p->wr, p->cmp_mode, p->cmp_func, p->lod_min, p->lod_max, p->lod_bias, p->aniso); }
}

// col != NULL 时把哑图填成那个颜色：采样采到 0 与采到 1 是两种完全不同的现场，
// 填成白色后「只剩贴图内容没进来」会显形成片纯色几何，「管线还坏了」才真是一片黑。
// A non-NULL col paints the dummy: sampling 0 and sampling 1 are two different scenes, so a white dummy shows up as flat-shaded
// geometry when only texture content is missing, while a still-black screen means the pipeline itself is broken further back.
// pat != 0 时不再填纯色，而是填一张 64x64 的横竖渐变（R=x、G=y、B 固定）：第 110 轮之后画面整幅纯白，
// 「白是采了白哑图」和「白是着色器算出来的」两种解释分不开。换成渐变以后一眼可判——出图里出现这个图案
// 就是哑图造成的，仍然纯白就跟贴图无关，直接转去查常量缓冲。渐变还顺带把 UV 方向与环绕暴露出来。
// pat paints a 64x64 horizontal/vertical ramp instead of a flat colour. Round 110's frame is uniformly white and "it sampled the
// white dummy" is indistinguishable from "the shader computed white"; with a ramp the pattern either shows up (dummies are the
// cause) or does not (go look at the constant buffers), and it exposes UV orientation and wrap as a side effect.
#define DUMMY_PAT 64
static int mk_dummy(VkImageType ty, VkFormat f, uint32_t layers, VkImageCreateFlags fl, VkImageAspectFlags aspect,
                    const float *col, int pat, VkImageView *ov)
{
    VkImage im;
    uint32_t side = (pat && ty == VK_IMAGE_TYPE_2D && f == VK_FORMAT_R8G8B8A8_UNORM) ? DUMMY_PAT : 1u;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.flags = fl;
    ii.imageType = ty;
    ii.format = f;
    ii.extent = (VkExtent3D){ side, side, ty == VK_IMAGE_TYPE_3D ? 1u : layers };
    ii.mipLevels = 1; ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (col ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
    ii.initialLayout = col ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkCreateImage(g_dev, &ii, NULL, &im) != VK_SUCCESS) { GRED("建哑图失败 型 %{public}d 格式 %{public}d", (int)ty, (int)f); return 0; }
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, im, &rq);
    int ti = find_type(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size; ai.memoryTypeIndex = ti >= 0 ? (uint32_t)ti : 0;
    VkDeviceMemory m;
    if (ti < 0 || vkAllocateMemory(g_dev, &ai, NULL, &m) != VK_SUCCESS || vkBindImageMemory(g_dev, im, m, 0) != VK_SUCCESS) {
        GRED("哑图绑内存失败"); vkDestroyImage(g_dev, im, NULL); return 0;
    }
    if (col) {   // 开局一次性填充：未定义 → 传递目标 → 着色只读 | one-shot boot fill: undefined -> transfer dst -> shader read-only
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_cp; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer cb;
        if (vkAllocateCommandBuffers(g_dev, &cai, &cb) != VK_SUCCESS) { GRED("哑图填充取命令缓冲失败"); return 0; }
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier br[2] = {
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
              im, { aspect, 0, 1, 0, layers } },
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
              im, { aspect, 0, 1, 0, layers } },
        };
        VkClearColorValue cc = { { col[0], col[1], col[2], col[3] } };
        VkBuffer pb = NULL; VkDeviceMemory pm = NULL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &br[0]);
        if (side > 1) {
            size_t nb = (size_t)side * side * layers * 4;
            void *mp = NULL;
            if (!mk_hostbuf((uint64_t)nb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &pb, &pm, &mp) || !mp) {
                GRED("渐变哑图的暂存缓冲没建成"); vkEndCommandBuffer(cb); return 0;
            }
            uint8_t *q = (uint8_t *)mp;
            for (uint32_t la = 0; la < layers; la++) for (uint32_t y = 0; y < side; y++) for (uint32_t x = 0; x < side; x++) {
                uint8_t *t = q + (((size_t)la * side + y) * side + x) * 4;
                t[0] = (uint8_t)(x * 255 / (side - 1)); t[1] = (uint8_t)(y * 255 / (side - 1));
                t[2] = 128; t[3] = 255;
            }
            VkBufferImageCopy rg = {
                .imageSubresource = (VkImageSubresourceLayers){ aspect, 0, 0, layers },
                .imageExtent = (VkExtent3D){ side, side, 1 } };
            vkCmdCopyBufferToImage(cb, pb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        } else {
            vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1,
                                 &(VkImageSubresourceRange){ aspect, 0, 1, 0, layers });
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &br[1]);
        vkEndCommandBuffer(cb);
        VkFenceCreateInfo fi_ = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fn; VkResult sr = VK_SUCCESS;
        VkResult fc = vkCreateFence(g_dev, &fi_, NULL, &fn);
        if (fc == VK_SUCCESS) {
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            sr = vkQueueSubmit(g_q, 1, &si, fn);
            if (sr == VK_SUCCESS) vkWaitForFences(g_dev, 1, &fn, VK_TRUE, 2000000000ull);
        }
        if (fc != VK_SUCCESS || sr != VK_SUCCESS) GRED("哑图填充提交失败（%{public}d）", (int)sr);
        if (fc == VK_SUCCESS) vkDestroyFence(g_dev, fn, NULL);
        vkFreeCommandBuffers(g_dev, g_cp, 1, &cb);
        if (pb) { vkDestroyBuffer(g_dev, pb, NULL); vkFreeMemory(g_dev, pm, NULL); }
    }
    VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    iv.image = im;
    iv.viewType = ty == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D
                : ty == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                : layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE
                : ty == VK_IMAGE_TYPE_2D && layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                : VK_IMAGE_VIEW_TYPE_2D;
    iv.format = f;
    iv.subresourceRange = (VkImageSubresourceRange){ aspect, 0, 1, 0, layers };
    if (vkCreateImageView(g_dev, &iv, NULL, ov) != VK_SUCCESS) { GRED("建哑图视图失败"); return 0; }
    return 1;
}

static int dummy_boot(void)
{
    if (g_zero) return 1;
    if (!mk_hostbuf(ZERO_SZ, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                          | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &g_zero, &g_zero_mem, NULL)) return 0;
    if (!mk_hostbuf(ZERO_SZ, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g_sup, &g_sup_mem, NULL)) return 0;
    g_sup_sz = ZERO_SZ;
    {   // support_buffer：Ryujinx 布局，只有 render_scale 要 1.0（nvn_mtl.m:2667 同一份）| only render_scale needs 1.0, same layout as nvn_mtl.m:2667
        uint8_t *p = NULL; vkMapMemory(g_dev, g_sup_mem, 0, ZERO_SZ, 0, (void **)&p);
        if (p) for (int i = 0; i < 73; i++) *(float *)(p + 192 + 16 * i) = 1.0f;
    }
    // 每种维数一张哑图：描述符的视图类型必须与着色器声明的维数同族，否则是非法用法（校验层不在，锅由驱动判）。
    // 颜色采样族默认纯黑（下面那条），缓冲维（拿坐标 / LUT）与深度填 0 —— 查表填白会把 UV 推到画外，反而看不出东西。
    // One dummy per dimension class: a view whose type disagrees with the shader's declaration is illegal usage - no validator, the driver decides.
    // The color-sampling dummies default to black (see below); the buffer-dim (coordinate / LUT) and depth dummies stay 0, since a white
    // lookup table would push UVs off-screen and hide the geometry instead of revealing it.
    static const float w4[4] = { 1, 1, 1, 1 }, z4[4] = { 0, 0, 0, 0 }, k4[4] = { 0, 0, 0, 1 };
    // 默认：哑图 = 不透明纯黑 1x1、不带渐变（第 111 轮真机定死）。彩色渐变会把「采到空图」画成一片花，
    // 和游戏内容混在一起；换纯黑后主菜单立刻显形，缺什么一眼看得见。位 33554432 退回彩色渐变，做 A/B 用。
    // The dummies default to opaque black 1x1 with no ramp (settled on device in round 111): a ramp paints every empty read as colour,
    // indistinguishable from content, while black made the real menu appear and left no doubt about which reads were empty.
    // Bit 33554432 restores the coloured ramp for the A/B.
    const float *c4 = (g_skip & 33554432) ? w4 : k4;
    const int dpat = (g_skip & 33554432) ? 1 : 0;
    if (!mk_dummy(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 0, VK_IMAGE_ASPECT_COLOR_BIT, c4, dpat, &g_dim_view[0])) return 0;
    if (!mk_dummy(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 2, 0, VK_IMAGE_ASPECT_COLOR_BIT, c4, dpat, &g_dim_view[1])) return 0;
    if (!mk_dummy(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 0, VK_IMAGE_ASPECT_COLOR_BIT, c4, 0, &g_dim_view[2])) return 0;
    if (!mk_dummy(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, c4, dpat, &g_dim_view[3])) return 0;
    if (!mk_dummy(VK_IMAGE_TYPE_1D, VK_FORMAT_R8G8B8A8_UNORM, 1, 0, VK_IMAGE_ASPECT_COLOR_BIT, z4, 0, &g_dim_view[4])) return 0;   // 缓冲维查表用的 1D 哑图 | the 1D dummy a buffer-dim lookup binds
    if (!mk_dummy(VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, 1, 0, VK_IMAGE_ASPECT_DEPTH_BIT, z4, 0, &g_shview)) return 0;
    mk_sampler(&g_smp_lin, 0); mk_sampler(&g_smp_cmp, 1);
    if (!g_smp_lin || !g_smp_cmp) { GRED("哑采样器没建起来"); return 0; }
    return 1;
}

// ---- 管线布局：按（顶点条目, 像素条目）对缓存 | pipeline layouts, keyed by the (vertex, fragment) entry pair ----
typedef struct { int vi, fi; VkPipelineLayout pl; } plyent;
#define PLY_N 384
static plyent g_ply[PLY_N]; static int g_ply_n;

static VkPipelineLayout ply_get(int vi, int fi)
{
    for (int i = 0; i < g_ply_n; i++) if (g_ply[i].vi == vi && g_ply[i].fi == fi) return g_ply[i].pl;
    if (g_ply_n >= PLY_N) { static int said; if (!said++) GRED("管线布局表满了（%{public}d）", PLY_N); return NULL; }
    VkDescriptorSetLayout lays[2] = { g_sho[vi].dsl, g_sho[fi].dsl };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 2; li.pSetLayouts = lays;
    VkPipelineLayout pl = NULL;
    if (vkCreatePipelineLayout(g_dev, &li, NULL, &pl) != VK_SUCCESS) { GRED("建管线布局失败（顶 %{public}d 像 %{public}d）", vi, fi); return NULL; }
    g_ply[g_ply_n++] = (plyent){ vi, fi, pl };
    return pl;
}

// ---- NVN → Vulkan 枚举表 | the NVN -> Vulkan enum tables ----
static VkPrimitiveTopology topo_of(int p)
{
    switch (p) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // 四边形 / 线环 / 补丁这些先不接 | quads, line loops and patches are not wired yet
    }
}
static VkCompareOp cmp_of(int f) { return (VkCompareOp)(f >= NVN_DEPTH_FUNC_NEVER && f <= NVN_DEPTH_FUNC_ALWAYS ? f - 1 : VK_COMPARE_OP_ALWAYS); }
static VkStencilOp stop_of(int o) { return (VkStencilOp)(o >= NVN_STENCIL_OP_KEEP && o <= NVN_STENCIL_OP_DECR_WRAP ? o - 1 : VK_STENCIL_OP_KEEP); }
// NVN 的混合因子编号跟 GL 一样（ZERO=1…SATURATE=0xB，SRC1_*=0x10…，CONSTANT_*=0x61…），Vulkan 的 VkBlendFactor 顺序却是
// DST_COLOR/ONE_MINUS_DST_COLOR 插在 SRC_ALPHA 前面。所以 0x5..0xB 这段绝不能做 f-1：
// SRC_ALPHA(0x5) 会被换成 DST_COLOR(4)、ONE_MINUS_SRC_ALPHA(0x6) 被换成 ONE_MINUS_DST_COLOR(5)，
// UI 的 src-alpha 透明混合直接变成 dst-modulate —— 半透明面板画成不透明亮纸、行标题被盖掉。逐条列死。
static VkBlendFactor bfac_of(int f)
{
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO;
    case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop_of(int e)   // Vulkan 叫 VkBlendOp（NVN / GL 的 equation 同名不同叫法）| Vulkan spells these VkBlendOp
{
    switch (e) {
    case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN;
    case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

// 顶点格式：NVN 编号 = 分量数 × 类型（1–4 分量各 12 个，0x01–0x30）；着色器声明的类型（cls）与 NVN 不符时换成同宽同数的整数族
// Vertex formats: the NVN code is (component count, type) - four groups of twelve from 0x01 to 0x30. When the shader's declared class
// disagrees with the NVN type the format is swapped to the integer family of the same width and component count.
static const VkFormat VF8F[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
static const VkFormat VF8S[4] = { VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM };
static const VkFormat VU8[4]  = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT };
static const VkFormat VI8[4]  = { VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT };
static const VkFormat VH[4]   = { VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT };
static const VkFormat VU16N[4]= { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM };
static const VkFormat VS16N[4]= { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
static const VkFormat VU16[4] = { VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT };
static const VkFormat VS16[4] = { VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT };
static const VkFormat VF32[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
static const VkFormat VU32[4] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT };
static const VkFormat VI32[4] = { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT };

static VkFormat vfmt_of2(int f, uint32_t *sz)
{
    *sz = 0;
    if (f >= 0x01 && f <= 0x30) {
        int idx = f - 1, nc = 1 + idx / 12, w = idx % 12;
        const VkFormat *fam;
        switch (w) {
        case 0: fam = VF8F; break;   case 1: fam = VF8S; break;   case 2: fam = VU8; break;   case 3: fam = VI8; break;
        case 4: fam = VH; break;     case 5: fam = VU16N; break;  case 6: fam = VS16N; break; case 7: fam = VU16; break;
        case 8: fam = VS16; break;   case 9: fam = VF32; break;   case 10: fam = VU32; break; default: fam = VI32; break;
        }
        *sz = (w <= 3 ? 1u : w <= 8 ? 2u : 4u) * (uint32_t)nc;
        return fam[nc - 1];
    }
    if (f == 0x3D || f == 0x3E) { *sz = 4; return VK_FORMAT_A2B10G10R10_UNORM_PACK32; }
    if (f == 0x3F) { *sz = 4; return VK_FORMAT_B10G11R11_UFLOAT_PACK32; }
    if (f == 0x9A || f == 0x99) { *sz = 4; return VK_FORMAT_B8G8R8A8_UNORM; }
    return 0;
}

typedef struct { VkFormat f; uint32_t off, binding; int constant; } vtx_plan;   // binding 16 = 常数零流 | binding 16 is the constant zero stream
#define VTX_CONST_BINDING 16

static void vtx_plan_for(int loc, int cls, vtx_plan *out)
{
    out->f = VK_FORMAT_UNDEFINED; out->off = 0; out->binding = VTX_CONST_BINDING; out->constant = 1;
    if (loc < 0 || loc >= 16 || loc >= S.nattr) return;
    va_st a = S.attr[loc];
    int st = a.stream;
    if (st < 0 || st > 15 || st >= S.nstream) return;
    uint32_t sz = 0;
    VkFormat m = vfmt_of2(a.fmt, &sz);
    if (!m) return;
    int idx = a.fmt - 1, w = (a.fmt >= 0x01 && a.fmt <= 0x30) ? idx % 12 : -1;
    int nc = w < 0 ? 4 : 1 + idx / 12;
    if (cls == 1) m = (w < 0 || w <= 3) ? VU8[nc - 1] : (w <= 8 ? VU16[nc - 1] : VU32[nc - 1]);
    else if (cls == 2) m = (w < 0 || w <= 3) ? VI8[nc - 1] : (w <= 8 ? VS16[nc - 1] : VI32[nc - 1]);
    else if (w == 7 || w == 8 || w == 10 || w == 11) return;   // 裸整数喂浮点输入：Vulkan 没有这种组合格式 | a raw integer format feeding a float input has no Vulkan equivalent
    uint32_t stride = S.stream[st].stride;
    if (stride && a.offset + sz > stride) return;   // 属性越出步长：交给常数流，别读别人的数据 | attribute outside the stride: constant stream, never read a neighbour's bytes
    out->f = m; out->off = stride ? a.offset : 0; out->binding = (uint32_t)st; out->constant = 0;
}

// ---- 管线缓存 | the pipeline cache ----
typedef struct {
    uint32_t vi, fi; uint64_t pk;
    uint32_t ncol, has_ds, topo, poly, ds, color, chmask, a2c, blend_en;
    uint64_t blend[8];
    va_st attr[16]; vs_st stream[16];
} pipe_key;
typedef struct { pipe_key k; VkPipeline p; } pipe_ent;
#define PIPE_N 4096
static pipe_ent g_pipe[PIPE_N]; static int g_pipe_n;
static uint32_t g_pipe_idx[4096];      // 键哈希 → 条目号 + 1 | key hash -> entry index + 1
static _Atomic uint64_t g_pipe_hit, g_pipe_new, g_pipe_fail;
static _Atomic uint64_t g_pipe_fail_scop, g_pipe_fail_plain;   // 失败里分「含 subgroup/原子/屏障」与「不含」两堆 | failures split by whether a scoped instruction is involved
// 头 24 条管线失败的现场：顶/像条目号 + 两段各做过哪些改写 + 拓扑 + VkResult。
// 汇总行已经很长，hilog 对单条消息有长度上限（第 72 轮的 note 就是这么被截掉的），所以每条单独打一行纯 ASCII。
// The first 24 pipeline failure scenes: vertex / fragment entry, the rewrite flags of each half, topology and VkResult.
// The summary line is already long and hilog caps a single message (that is what ate round 72's note), so each scene gets its own short ASCII line.
typedef struct { int16_t vi, fi; uint8_t fv, ff; int8_t prim; int32_t r; int32_t pv, pf; } pf_ent;
static pf_ent g_pf[24];
static _Atomic int g_pf_n;

static uint32_t pk_hash(const pipe_key *k)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)k;
    for (size_t i = 0; i + 8 <= sizeof *k; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; }
    return (uint32_t)(h >> 40) & 4095u;
}

static VkPipeline pipe_get(int vi, int fi, int prim, VkRenderPass rp, uint64_t passk);

static void pipe_key_fill(pipe_key *k, int vi, int fi, int prim, VkRenderPass rp, uint64_t passk)
{
    memset(k, 0, sizeof *k);   // 补齐字节必须为 0，否则 memcmp 认不出同一条 | padding must be zero or the memcmp never matches
    k->vi = (uint32_t)vi; k->fi = (uint32_t)fi; k->pk = passk ^ (uint64_t)(uintptr_t)rp;
    k->ncol = g_p.ncol; k->has_ds = g_p.has_ds;
    k->topo = (uint32_t)prim;
    k->poly = *(const uint32_t *)&S.poly;
    k->ds = (uint32_t)((*(const uint64_t *)&S.ds) ^ ((*(const uint64_t *)&S.ds) >> 32));   // ds_st 就是一个 uint64 | ds_st is a single uint64
    k->color = *(const uint32_t *)&S.color;
    k->chmask = S.chmask.mask; k->a2c = S.ms.a2c; k->blend_en = S.color.blend_en;
    for (int i = 0; i < 8; i++) memcpy(&k->blend[i], &S.blend[i], sizeof S.blend[i]);
    // 只有着色器真声明的属性进键：其余槽位改了不该让用户重编管线 | only attributes the shader declares take part in the key
    for (int v = 0; v < g_shb_e[vi].nvi; v++) {
        int loc = g_shb_vi[g_shb_e[vi].vi_off + v].loc;
        if (loc < 16) { k->attr[loc] = S.attr[loc]; int st = S.attr[loc].stream; if (st < 16) k->stream[st] = S.stream[st]; }
    }
}

static void pipe_put(const pipe_key *k, uint32_t h, VkPipeline p)
{
    if (g_pipe_n >= PIPE_N) return;
    g_pipe[g_pipe_n] = (pipe_ent){ *k, p };
    uint32_t slot = h;
    for (int t = 0; t < 32; t++) { uint32_t q = (h + t) & 4095u; if (!g_pipe_idx[q]) { slot = q; break; } }
    g_pipe_idx[slot] = (uint32_t)(++g_pipe_n);
}

// 现场二分：只拿这一个游戏顶点段建一条 rasterizerDiscard 的管线。Vulkan 允许丢弃光栅时不带像素段，
// 所以这条探针能建成就说明顶点段自己编得过，锅在像素那半（第 72 轮 46/46 全败，必须先把两半分开）。
// Failure bisect: build a rasterizerDiscard pipeline from this single game vertex module. Vulkan allows no fragment stage
// when rasterization is discarded, so a success means the vertex half compiles on its own and the blame falls on the
// fragment half (round 72 failed 46 of 46 - the two halves had to be separated first).
static int32_t probe_stage(int vi, int fi, int tri, VkPipelineLayout pl, VkRenderPass rp)
{
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    int ns = 1;
    if (tri) {
        // tri=1：自带满屏三角形的顶点段 + 游戏像素段。像素段没法单独立管线（Vulkan 必须有顶点段），
        // 借一个已知能编过的顶点段，就把「游戏像素段本身编不过」和「两段的接口对不上」分开了。
        // A fragment stage cannot stand alone (Vulkan requires a vertex stage), so borrow the built-in triangle's vertex module:
        // that separates "the game's fragment does not compile" from "the two halves' varyings do not match".
        if (!g_tri_vs || !g_sho[fi].mod) return -999;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_tri_vs; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_sho[fi].mod; st[1].pName = "main";
        ns = 2;
    } else {
        if (!g_sho[vi].mod) return -999;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_sho[vi].mod; st[0].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    // 两种探针都丢弃光栅化输出：像素段照样参与编译，但不碰交换链格式，失败原因就只剩着色编译本身
    // Both probes discard raster output: the fragment stage still compiles, but no attachment format is involved
    rs.rasterizerDiscardEnable = VK_TRUE; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo mss = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    mss.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba0 = { VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, VK_FALSE, 0, 0, 0, 0, 0 };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = g_p.ncol ? g_p.ncol : 1; cbs.pAttachments = &cba0;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dys = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dys.dynamicStateCount = 2; dys.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gi.stageCount = (uint32_t)ns; gi.pStages = st;
    gi.pVertexInputState = &vis; gi.pInputAssemblyState = &ias; gi.pViewportState = &vps;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &mss; gi.pDepthStencilState = &dss;
    gi.pColorBlendState = &cbs; gi.pDynamicState = &dys;
    gi.layout = pl; gi.renderPass = rp; gi.subpass = 0;
    VkPipeline p = NULL;
    VkResult r = vkCreateGraphicsPipelines(g_dev, NULL, 1, &gi, NULL, &p);
    if (p) vkDestroyPipeline(g_dev, p, NULL);
    return (int32_t)r;
}

static VkPipeline pipe_get(int vi, int fi, int prim, VkRenderPass rp, uint64_t passk)
{
    pipe_key k; pipe_key_fill(&k, vi, fi, prim, rp, passk);    uint32_t h = pk_hash(&k);
    for (int t = 0; t < 32; t++) {
        uint32_t q = (h + t) & 4095u;
        if (!g_pipe_idx[q]) break;
        if (g_pipe_idx[q] && !memcmp(&g_pipe[g_pipe_idx[q] - 1].k, &k, sizeof k)) { atomic_fetch_add(&g_pipe_hit, 1); return g_pipe[g_pipe_idx[q] - 1].p; }
    }
    if (g_pipe_n >= PIPE_N) { static int said; if (!said++) GRED("管线缓存满了（%{public}d）", PIPE_N); return NULL; }
    VkPipelineLayout pl = ply_get(vi, fi);
    if (!pl) return NULL;
    // 顶点输入：按着色器声明的属性建，绑不动的一律走 16 号常数零流
    // Vertex input: one description per declared attribute; anything we cannot wire falls back to binding 16, a zero stream.
    const shb_entry *ve = &g_shb_e[vi];
    VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDescription vb[17];
    uint32_t nva = 0; uint32_t used = 1u << VTX_CONST_BINDING;
    for (int v = 0; v < ve->nvi && nva < 32; v++) {
        int loc = g_shb_vi[ve->vi_off + v].loc, cls = g_shb_vi[ve->vi_off + v].cls;
        vtx_plan p; vtx_plan_for(loc, cls, &p);
        if (p.constant) {
            static int said;   // 回退也按 cls 给宽度：整数输入配整数格式，否则是非法组合 | the fallback keeps the declared class: an integer input needs an integer format
            if (!said++) GVL("[vk] 顶点属性 %{public}d 接不上（NVN 格式 %{public}#x），走常数零流", loc, S.attr[loc < 16 ? loc : 0].fmt);
            va[nva++] = (VkVertexInputAttributeDescription){ (uint32_t)loc, VTX_CONST_BINDING,
                cls == 1 ? VK_FORMAT_R32G32B32A32_UINT : cls == 2 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
        } else {
            va[nva++] = (VkVertexInputAttributeDescription){ (uint32_t)loc, p.binding, p.f, p.off };
            used |= 1u << p.binding;
        }
    }
    uint32_t nvb = 0;
    vb[nvb++] = (VkVertexInputBindingDescription){ VTX_CONST_BINDING, 0, VK_VERTEX_INPUT_RATE_INSTANCE };
    for (int st = 0; st < 16; st++) if (used & (1u << st))
        vb[nvb++] = (VkVertexInputBindingDescription){ (uint32_t)st, S.stream[st].stride,
            S.stream[st].stride ? (S.stream[st].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX) : VK_VERTEX_INPUT_RATE_INSTANCE };
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vis.vertexBindingDescriptionCount = nvb; vis.pVertexBindingDescriptions = vb;
    vis.vertexAttributeDescriptionCount = nva; vis.pVertexAttributeDescriptions = va;
    VkPrimitiveTopology tp = topo_of(prim);
    if (tp == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) { static int saidt; if (!saidt++) GVL("[vk] 拓扑 %{public}d 这刀不接", prim); return NULL; }
    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = tp;
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkCullModeFlags cull = S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT
                        : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT
                        : S.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    VkPolygonMode pmode = S.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT
                        : S.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = pmode; rs.cullMode = cull;
    // 第 99 轮：「输入1」整张是黑的，两种经典全黑成因各给一个开关，一次构建做 A/B——剔背面剔光了 / 深度测试全灭。
    // Round 99: the scene input is all black. Two classic causes of exactly that, each behind its own bit so one build A/B's both:
    // back-face culling eating every triangle (winding), or the depth test rejecting everything.
    if (g_skip & 262144) rs.cullMode = VK_CULL_MODE_NONE;
    // 第 101 轮实测：位 262144 关掉剔背面后「输入2」读回 nz=1433313/1440000、均值 253（前一档全零），位 524288 关深度测试无变化
    // —— 剔背面剔光了，深度无罪。枚举与 Metal 那份同源、三元表达式语义一致，所以反的不是枚举，是帧缓冲手性：
    // Metal 底左原点 y 向上、Vulkan 顶左 y 向下，同一段 SPIR-V 在两边视觉绕序天然相反，照抄映射必把三角形全剔。
    // Round 101: bit 262144 (no cull) makes the scene input non-black, bit 524288 (no depth test) changes nothing, so culling is the
    // killer. The enums are shared with the Metal build and the expression is semantically identical, so what differs is framebuffer
    // handedness (Metal y-up bottom-left vs Vulkan y-down top-left): the same SPIR-V winds the opposite way. Hence the inversion.
    rs.frontFace = S.poly.front == NVN_FRONT_FACE_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (g_skip & 1048576) rs.frontFace = S.poly.front == NVN_FRONT_FACE_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = S.poly.offset_en ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo mss = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    mss.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;   // 通道附件全是 1 采样（rp_get 写死的），所以多采样的绘制只能跳过 | rp_get attaches everything at 1 sample, so MSAA draws are skipped
    mss.alphaToCoverageEnable = S.ms.a2c ? VK_TRUE : VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = S.ds.dt ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = S.ds.dw ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp = cmp_of(S.ds.df);
    dss.stencilTestEnable = S.ds.st ? VK_TRUE : VK_FALSE;
    if (g_skip & 524288) dss.depthTestEnable = VK_FALSE;   // 位 524288：不测深度（深度全灭那种黑）| bit 524288: never reject a fragment on depth
    dss.front = (VkStencilOpState){ stop_of(S.ds.fail_f), stop_of(S.ds.zpass_f), stop_of(S.ds.zfail_f), cmp_of(S.ds.sf_f), 0xFF, 0xFF, 0 };
    dss.back  = (VkStencilOpState){ stop_of(S.ds.fail_b), stop_of(S.ds.zpass_b), stop_of(S.ds.zfail_b), cmp_of(S.ds.sf_b), 0xFF, 0xFF, 0 };
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t m = (S.chmask.mask >> (4 * i)) & 0xF;
        cba[i] = (VkPipelineColorBlendAttachmentState){0};
        cba[i].colorWriteMask = (m & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | (m & 2 ? VK_COLOR_COMPONENT_G_BIT : 0)
                              | (m & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) | (m & 8 ? VK_COLOR_COMPONENT_A_BIT : 0);
        if (i < g_p.ncol && (S.color.blend_en >> i) & 1) {
            const blend_st *b = &S.blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = bfac_of(b->src_rgb); cba[i].dstColorBlendFactor = bfac_of(b->dst_rgb);
            cba[i].srcAlphaBlendFactor = bfac_of(b->src_a); cba[i].dstAlphaBlendFactor = bfac_of(b->dst_a);
            cba[i].colorBlendOp = bop_of(b->eq_rgb); cba[i].alphaBlendOp = bop_of(b->eq_a);
            // 位 268435456：把源因子一律顶成 ONE（B45 探针）。B44 修对混合表以后，UI 的半透明层
            // 从「画错成平涂」变成「整笔不画」⇒ 要么源 alpha 其实是 0，要么这些笔根本不是 SRC_ALPHA 那对。
            // 顶成 ONE 就不用源 alpha 了：元素回来 = 源 alpha 的锅；还是空 = 混合态另有原因。
            // 位号从 33554432 挪来：那一位现在归「哑图退回彩色渐变」。
            if (g_skip & 268435456) { cba[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; }
        }
    }
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = g_p.ncol; cbs.pAttachments = cba;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_LINE_WIDTH };
    VkPipelineDynamicStateCreateInfo dys = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dys.dynamicStateCount = 5; dys.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_sho[vi].mod; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_sho[fi].mod; st[1].pName = "main";
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vis; gi.pInputAssemblyState = &ias; gi.pViewportState = &vps;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &mss; gi.pDepthStencilState = &dss;
    gi.pColorBlendState = &cbs; gi.pDynamicState = &dys;
    gi.layout = pl; gi.renderPass = rp; gi.subpass = 0;
    VkPipeline p = NULL;
    VkResult r = vkCreateGraphicsPipelines(g_dev, NULL, 1, &gi, NULL, &p);
    if (r != VK_SUCCESS) {
        int sc = g_sho[vi].scop || g_sho[fi].scop;   // 这一对里有没有 subgroup / 原子 / 屏障指令 | does this pair contain subgroup / atomic / barrier ops
        atomic_fetch_add(&g_pipe_fail, 1);
        atomic_fetch_add(sc ? &g_pipe_fail_scop : &g_pipe_fail_plain, 1);
        int pn = atomic_load(&g_pf_n);
        if (pn < (int)(sizeof g_pf / sizeof g_pf[0])) {
            int seen = 0;
            for (int t = 0; t < pn; t++) if (g_pf[t].vi == vi && g_pf[t].fi == fi) { seen = 1; break; }
            if (!seen) {   // 每一对只记一次，并把顶点段单独拉出来试编（区分两半）| one record per pair, plus the vertex-only bisect
                int32_t pv = probe_stage(vi, fi, 0, pl, rp);
                int32_t pf = probe_stage(vi, fi, 1, pl, rp);
                g_pf[pn] = (pf_ent){ (int16_t)vi, (int16_t)fi, g_sho[vi].feat, g_sho[fi].feat, (int8_t)prim, (int32_t)r, pv, pf };
                atomic_store(&g_pf_n, pn + 1);
            }
        }
        pipe_put(&k, h, NULL);   // 失败也进缓存：着色编译失败是确定的，同一对不该再编第二万次 | a compile failure is deterministic - never recompile the same pair
        return NULL;
    }
    pipe_put(&k, h, p);
    atomic_fetch_add(&g_pipe_new, 1);
    if (g_pipe_n <= 12 || (g_pipe_n % 200) == 0) GVL("[vk] 新管线 %{public}d 条（顶 %{public}d 像 %{public}d 拓 %{public}d 命中 %{public}llu）",
        g_pipe_n, vi, fi, prim, (unsigned long long)g_pipe_hit);
    return p;
}

// ---- 描述符集：每段一组（顶点 0、像素 1），脏了才重编 | one set per stage, rebuilt only when that stage's bindings changed ----
static VkDescriptorPool g_dpool;
static VkDescriptorSet g_cur_set[2];
static int g_cur_vi = -1, g_cur_fi = -1, g_sets_valid;
static _Atomic uint64_t g_set_oom;

// 绑图三条路分开记：实 = 绑上游戏那张贴图的视图，哑 = 槽里句柄有效但图还没建（或深度 / shadow），
// 无号 = 槽里根本没有句柄，缓冲 = 着色器按缓冲维取数（查表），这刀先绑 1D 哑图并记下真图长什么样。
// Three binding outcomes counted separately: real view / handle resolves but no VkImage yet (or depth, or shadow) / no handle at all
// / buffer-dim lookup, which binds a 1D dummy for now and logs what the real texture looks like.
static _Atomic uint64_t g_bind_real, g_bind_dummy, g_bind_none, g_bind_buf, g_bind_lay;
// 哑的分桶计数器 g_b_* 声明在前面（统计块要先用）| the dummy-bucket counters g_b_* are declared earlier, the stats block uses them first

static int set_fill(VkDescriptorSet set, int ei, int stage)
{
    const shb_entry *e = &g_shb_e[ei];
    VkWriteDescriptorSet wr[64]; VkDescriptorBufferInfo bi[64]; VkDescriptorImageInfo ii[64];
    int nb = 0, ni = 0, nw = 0;
    for (int i = 0; i < e->nb && nw < 64; i++) {
        const shb_bind *b = &g_shb_b[e->bind_off + i];
        int reg = b->nvn_reg;
        wr[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr[nw].dstSet = set; wr[nw].dstBinding = b->binding; wr[nw].descriptorCount = 1;
        wr[nw].descriptorType = desc_type(b->kind);
        if (b->kind == SHB_COMBINED) {
            int dim = b->vtype & 15, sh = (b->vtype & 0x10) ? 1 : 0;
            // 贴图槽里是合并句柄：低 20 位贴图号、高位采样器号（nvn_mtl.m:2741 同一拆法）。
            // 槽号有两个候选寄存器：combined 绑定同时带贴图 t 与采样器 s，nvn_reg 存的是 s，t 在 bank（= t+1，0 = 旧库没有）。
            // 第 86 轮实测「着色器查的槽全是 0-8 而游戏只往 10-24 填句柄」就是因为旧库里 t 没打包进来，只能退回 nvn_reg。
            // 三个候选各数一遍命中（bank / SPIR-V binding 号 / reg），新库上线后 bank 应该吃掉绝大多数。
            // The slot holds a combined handle: texture id in the low 20 bits, sampler id above it (same split as nvn_mtl.m:2741).
            // A combined binding carries two registers: t (the texture) rides in bank as t+1, s (the sampler) is nvn_reg.
            // Round 86 measured shaders probing slots 0-8 while the game only filled 10-24 because an old library has no t,
            // leaving the fallback of using s as a texture slot. All three candidates are tallied so the new library's
            // hit rate is visible rather than assumed.
            int tslot = reg;
            if (stage >= 0 && stage < 6 && b->nvn_kind == SHB_N_SAMP) {
                int hb = b->bank > 0 && b->bank - 1 < 128 && S.tex[stage][b->bank - 1];
                int hd = b->binding < 128 && S.tex[stage][b->binding];
                if (hb) { atomic_fetch_add(&g_cand_bank, 1); tslot = b->bank - 1; }
                else if (hd) { atomic_fetch_add(&g_cand_bind, 1); tslot = b->binding; }
                else if (reg >= 0 && reg < 128 && S.tex[stage][reg]) atomic_fetch_add(&g_cand_reg, 1);
                else atomic_fetch_add(&g_cand_none, 1);
                // 三个候选全空：点名一次这个程序的槽号/库里的 bank/binding/reg，看地面那笔到底找的是哪个槽
                // All three candidates empty: name this program's slot numbers once, to see which slot the terrain draw asks for
                if (!hb && !hd && !(reg >= 0 && reg < 128 && S.tex[stage][reg])) {
                    static _Atomic int nnn; int kk = atomic_fetch_add(&nnn, 1);
                    if (kk < 24) GVL("[vk] 槽全空 key0=%{public}s st%{public}d bank=%{public}d binding=%{public}d reg=%{public}d｜槽内 b=%{public}llu d=%{public}llu r=%{public}llu",
                                     g_cur_prog ? g_cur_prog->key[0] : "?", stage, b->bank, b->binding, reg,
                                     (unsigned long long)(b->bank > 0 && b->bank - 1 < 128 ? (S.tex[stage][b->bank - 1] & 0xfffff) : 0ull),
                                     (unsigned long long)(b->binding < 128 ? (S.tex[stage][b->binding] & 0xfffff) : 0ull),
                                     (unsigned long long)(reg >= 0 && reg < 128 ? (S.tex[stage][reg] & 0xfffff) : 0ull));
                }
            }
            uint64_t texh = (stage >= 0 && stage < 6 && tslot >= 0 && tslot < 128 &&
                             (b->nvn_kind == SHB_N_SAMP || b->nvn_kind == SHB_N_SAMPT))
                            ? S.tex[stage][tslot] : 0;
            int tid = (int)(texh & 0xfffff);
            vktex *v = tid ? tex_of_tid(tid) : NULL;
            if (!v && tid && !(g_skip & 32768)) v = tex_make_tid(tid);   // 按需建图（第 92 轮「没图」占哑图 95%）
            // 打包贴图要能重传：游戏会把新素材写进仍在被采样的那张池内存（开局那段视频、LOADING 那条横幅
            // 都是这个形状——第 111 轮读回三帧逐字节相同、屏幕 60→100 s 两张截图 md5 一模一样 = 冻在头一帧）。
            // NVN 的语义是「描述符指向池内存，GPU 每次读的都是当前内容」，所以上传后把覆盖页设只读，
            // 游戏一写就缺页 → 代号前进；这里只比代号（一次原子读），代号动了才逐页查脏、排一次通道外重传。
            // Packaged textures have to be re-uploaded: the game writes new material into pool memory that is still being sampled
            // (the boot video and the LOADING banner are exactly this shape - round 111 read back three byte-identical frames and the
            // 60 s / 100 s screenshots hashed the same, i.e. frozen on frame one). NVN's semantics are "the descriptor points at pool
            // memory and the GPU reads the current content", so the covered pages go read-only after the upload and a write faults and
            // bumps the generation. Here only the generation is compared (one atomic load); pages are scanned once it moved.
            if (v && v->uploaded && v->pkg_range && !v->pkg_dirty && !(g_skip & 67108864)) {
                if (v->pkg_stale) { v->pkg_stale = 0; pkg_queue(v); }   // 新生命：不查脏，直接按「游戏重写过这块」补一次搬运
                else {
                    uint64_t now = nvn_dirty_now();
                    if (now != v->pkg_seen) {
                        v->pkg_seen = now;
                        if (nvn_dirty_since(v->d.mem, v->pkg_range, v->pkg_stamp)) { v->pkg_dirty = 1; pkg_queue(v); }
                    }
                    // 缺页没响过（代号没动）不等于池里没写：本机一轮只缺页两次，这里按内容比对补上这条判据
                    // A silent generation is not proof the bytes stood still - this device faults twice a round, so compare content too.
                    else if (g_poolwatch && v->ph_st != g_presents) {
                        v->ph_st = g_presents;
                        uint64_t h = pool_hash(v->d.mem, v->pkg_range);
                        if (v->ph_last && h != v->ph_last) {
                            atomic_fetch_add(&g_pw_chg, 1);
                            v->ph_last = h;
                            static uint64_t pw_frame; static int pw_n;
                            if (pw_frame != g_presents) { pw_frame = g_presents; pw_n = 0; }
                            if (pw_n >= 8) atomic_fetch_add(&g_pw_cap, 1);
                            else { pw_n++; v->pkg_dirty = 1; atomic_fetch_add(&g_pw_que, 1); pkg_queue(v); }
                        } else { if (!v->ph_last) v->ph_last = h; atomic_fetch_add(&g_pw_same, 1); }
                    }
                }
            }
            VkImageView iv = NULL;
            VkImageLayout lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            // 影子采样要真深度视图：本通道正把这张深度图当附件用时不能同趟读它（自己依赖没声明就是竞态），那种情况照旧走哑图。
            // A shadow read needs the real depth view, except when this very pass has that image attached - reading an attachment
            // in its own pass without a self-dependency is a race, so that case keeps the dummy.
            int in_att = v && g_p.rp && (v == g_p.dv);
            if (v && v->view && v->is_depth && sh && !in_att && !(g_skip & 8192) && tex_has_content(v)) {
                iv = v->view; lay = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
                atomic_fetch_add(&g_bind_real, 1);
                atomic_fetch_add(&g_b_shadow, 1);
                if (stage >= 0 && stage < 6) atomic_fetch_add(&g_bind_real_st[stage], 1);
            } else if (v && v->view && !v->is_depth && !sh && !(g_skip & 16384) && tex_has_content(v)) {
                iv = v->view;
                atomic_fetch_add(&g_bind_real, 1);
                if (stage >= 0 && stage < 6) atomic_fetch_add(&g_bind_real_st[stage], 1);
                if (v->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) atomic_fetch_add(&g_bind_lay, 1);
                if (!v->uploaded) {
                    atomic_fetch_add(&g_b_neverup, 1);
                    // 点名：进世界后地面整片纯绿，怀疑地表 atlas 属于「绑了但没传」。前 24 张逐张打身份。
                    // Name them: the world's ground is flat green and the terrain atlases may be these bind-but-never-uploaded
                    // images. Print the identity of the first 24.
                    static _Atomic int nu; int k = atomic_fetch_add(&nu, 1);
                    if (k < 24)
                        GVL("[vk] 没传过就绑 tid=%{public}d %{public}ux%{public}u 格%{public}d 级%{public}u serial=%{public}llu 画过%{public}d 传过%{public}d 建过%{public}llu",
                            tid, v->w, v->h, (int)v->fmt, v->levels, (unsigned long long)v->serial, v->drawn,
                            v->uploaded, (unsigned long long)atomic_load(&g_tex_make));
                }
                static int saidr;
                if (saidr < 10) {
                    saidr++;
                    GVL("[vk] 真绑%{public}d st%{public}d 槽%{public}d tid=%{public}d %{public}u x %{public}u 层%{public}u 级%{public}u 格式%{public}d 传过%{public}d serial=%{public}llu",
                        saidr - 1, stage, tslot, tid, v->w, v->h, v->layers, v->levels, (int)v->fmt, v->uploaded,
                        (unsigned long long)v->serial);
                }
            } else if (tid) {
                atomic_fetch_add(&g_bind_dummy, 1);
                // 「空图」到底是谁：按 serial 去重各报一次，下一刀才知道该给哪几张补内容。
                // Report each distinct empty serial once so the next cut knows which images to feed.
                if (v && !tex_has_content(v)) {
                    atomic_fetch_add(tex_sibling_content(v) ? &g_dy_sib : &g_dy_nosib, 1);
                    static uint64_t seen_es[40]; static int seen_en;
                    int k; for (k = 0; k < seen_en; k++) if (seen_es[k] == v->serial) break;
                    if (k == seen_en && seen_en < 40) {
                        seen_es[seen_en++] = v->serial;
                        // 池里到底有没有字节：有 ⇒ 是我们没搬（缺上传/解排布）；没有 ⇒ 内容压根没走内存这条路（该找别的写入点）
                        // Are there bytes in the guest pool at all? Yes = we failed to move them; no = the content never went this way.
                        uint64_t nz = 0;
                        if (v->d.mem && v->d.pool) {
                            // 第 107 轮这行把 d.off 又加了一遍：t->mem 本身已经指到池内这张图的开头，多加一次读到的是池里的空洞，
                            // 于是「非零 0」是个假阴性。取源方式与 mtl_read_texels 对齐（指针 = mem + 该 mip 级偏移，级别 0 就是 mem）。
                            // Round 107 added d.off on top of t->mem, but t->mem already points at this texture inside the pool, so that
                            // "all zero" reading was a false negative. Match mtl_read_texels: pointer = mem + level offset (level 0 = mem).
                            size_t av = (size_t)(v->d.pool->size > (uint64_t)v->d.off ? v->d.pool->size - (uint64_t)v->d.off : 0);
                            size_t sn = av < 8192 ? av : 8192;
                            const uint8_t *q = v->d.mem;
                            for (size_t b = 0; b < sn; b++) if (q[b]) nz++;
                        }
                        GVL("[vk] 空图%{public}d serial=%{public}llu tid=%{public}d 槽%{public}d st%{public}d %{public}u x %{public}u 层%{public}u 级%{public}u 格式%{public}d 深度%{public}d 目标%{public}d｜池内存%{public}d 非零%{public}llu/%{public}u 池大小%{public}llu 偏移%{public}llu",
                            seen_en - 1, (unsigned long long)v->serial, tid, tslot, stage, v->w, v->h, v->layers, v->levels,
                            (int)v->fmt, v->is_depth, v->d.d.target,
                            v->d.mem != NULL && v->d.pool != NULL, (unsigned long long)nz,
                            (unsigned)(v->d.mem && v->d.pool ? (v->d.pool->size - (uint64_t)v->d.off < 8192 ?
                                       (unsigned)(v->d.pool->size - (uint64_t)v->d.off) : 8192u) : 0u),
                            v->d.pool ? (unsigned long long)v->d.pool->size : 0ull, (unsigned long long)v->d.off);
                    }
                }
                // 排进打包上传队列：这张有池存储，像素一直躺在客体内存里没人搬（第 109 轮：不再要求 has_layout）
                // Queue it for the packaged upload: pool storage present, the pixels just sat in guest memory unmoved
                // (round 109: has_layout is no longer required - the guest writes these with the legacy WriteTexels).
                if (v && !tex_has_content(v)) {
                    emp_note(v);
                    if (v->d.mem) pkg_queue(v);
                }
                // 哑分三种，下一刀才知道该搬像素还是该建视图：压根没 VkImage / 是深度图但这条不是影子 / 影子却给了颜色图
                // Three flavours of dummy, so the next cut knows whether to move pixels or to build views: no VkImage at all /
                // a depth image on a non-shadow read / a shadow read whose handle resolved to a colour image.
                atomic_fetch_add(!v ? &g_b_noimg : (!tex_has_content(v) ? &g_b_empty
                                 : (v->is_depth ? &g_b_depth_plain : (sh ? &g_b_sh_color : &g_b_forced))), 1);
            } else { atomic_fetch_add(&g_bind_none, 1); if (reg >= 0 && reg < 32) atomic_fetch_add(&g_bind_miss[reg], 1); }
            if (dim == SHB_DIM_BUFFER) {   // 缓冲维（查表）：着色器已改成 1D，视图必须同为 1D；真图的 1D 视图是下一刀
                atomic_fetch_add(&g_bind_buf, 1);
                iv = NULL;
                static int saidb;
                if (saidb < 6 && tid) {
                    saidb++;
                    pthread_mutex_lock(&g_regm);
                    treg_t rg = (g_treg && tid < g_ntreg && g_treg[tid].ok) ? g_treg[tid] : (treg_t){0};
                    pthread_mutex_unlock(&g_regm);
                    GVL("[vk] 缓冲维绑定 条%{public}d st%{public}d 槽%{public}d tid=%{public}d NVN目标%{public}d 格式%{public}d %{public}u x %{public}u 有图%{public}d（先绑 1D 哑图）",
                        ei, stage, reg, tid, rg.t.d.target, rg.t.d.format, rg.t.d.width, rg.t.d.height, v ? 1 : 0);
                }
            }
            int dmy = !iv;
            if (!iv) { iv = sh ? g_shview : (dim <= SHB_DIM_BUFFER ? g_dim_view[dim] : g_dim_view[0]); lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
            // 第 96 轮实测：上屏那条通道每帧只有 1 绘、读回 RGB 全零但 alpha 有覆盖率 ⇒ 这一绘采进去的东西是零。
            // 把「挂得出图的通道」里每一条贴图绑定的判据打出来，看清楚它到底绑了真图还是哑图。
            // Round 96: the on-screen pass holds one draw per frame and its readback has RGB exactly 0 with real alpha coverage,
            // so that draw samples nothing. Print every texture binding made while a present source is attached.
            static int g_said_sp;
            static int g_said_dy;
            if (g_att_src) atomic_fetch_add(dmy ? &g_spb_dummy : &g_spb_real, 1);
            if (g_att_src && !dmy && v && !v->is_depth) {
                // 记下上屏那条通道采的两张真图，读回窗口按奇偶轮流读它们：先分清「输入就是黑的」还是「输出被吃掉」
                // Remember the two real inputs of the on-screen pass so the readback window can alternate over them:
                // is the input itself black, or is the output being thrown away?
                if (!g_sp_tex[0]) g_sp_tex[0] = v;
                else if (g_sp_tex[0] != v && !g_sp_tex[1]) g_sp_tex[1] = v;
            }
            if (g_att_src && (dmy ? g_said_dy < 24 : g_said_sp < 64)) {
                // 哑图那条单独限次：第 118 轮 24 条上限全被开头的真图占满，之后「上屏通道采了空图」一条都没记下来
                // The dummy case gets its own budget - round 118's shared 24-line cap was consumed by the opening real binds,
                // so every later "the on-screen pass sampled an empty" went unrecorded.
                if (dmy) g_said_dy++; else g_said_sp++;
                char rb[224];
                snprintf(rb, sizeof rb, "[vk] 上屏通道绑定 %d/%d st%d 槽%d tid=%d 有图%d 有内容%d 深度%d 影子%d 维%d s%llu %ux%u f%d 传%d 画%d → %s",
                         g_said_sp, g_said_dy, stage, reg, tid, v ? 1 : 0, tex_has_content(v), v ? v->is_depth : -1, sh, dim,
                         v ? (unsigned long long)v->serial : 0ull, v ? v->w : 0u, v ? v->h : 0u,
                         v ? (int)v->d.d.format : 0, v ? v->uploaded : -1, v ? v->drawn : -1,
                         dmy ? "哑图" : "真图");
                GVL("%{public}s", rb);
            }
            // 采样器号：合并绑定带的两个寄存器里，s 那格进的是采样器表 S.samp（句柄 = 号<<20，hle_nvn.c:181）；
            // 另一路是贴图槽句柄的高位（iOS 那边 SAMPT 的拆法）。哪个登记过就用哪个，都没有退回哑采样器。
            // The sampler ID: of the two registers a combined binding carries, s indexes the sampler table S.samp
            // (handle = id << 20, hle_nvn.c:181); the other route is the texture slot handle's high bits (iOS' SAMPT split).
            // Whichever one is registered wins, and both empty falls back to the dummy sampler.
            int sid = 0;
            if (stage >= 0 && stage < 6 && reg >= 0 && reg < 128) sid = (int)(S.samp[stage][reg] >> 20);
            if (sid && !samp_ok(sid)) sid = 0;
            if (!sid) sid = (int)(texh >> 20);
            VkSampler sm = sid ? samp_get(sid, sh) : NULL;
            ii[ni] = (VkDescriptorImageInfo){0};
            ii[ni].sampler = sm ? sm : (sh ? g_smp_cmp : g_smp_lin);
            ii[ni].imageView = iv;
            ii[ni].imageLayout = lay;   // 真深度视图停在 DEPTH_READ_ONLY（通道终态），哑图停在 SHADER_READ_ONLY：两套布局按来源分别声明
            wr[nw].pImageInfo = &ii[ni++];
        } else {
            VkBuffer buf = g_zero; uint64_t off = 0, range = 4096;
            uint32_t lim = b->kind == SHB_SSBO ? g_lim_ssbo : g_lim_ubo;
            const p_bind_buf *u = NULL;
            switch (b->nvn_kind) {
            case SHB_N_SUPPORT: buf = g_sup; range = g_sup_sz; break;
            case SHB_N_C1: if (g_sho[ei].c1b) { buf = g_sho[ei].c1b; range = g_sho[ei].c1sz; } break;
            case SHB_N_CBUF: if (reg >= 0 && reg < 16) u = &S.ubo[stage][reg]; break;
            case SHB_N_SBUF: if (reg >= 0 && reg < 16) u = &S.ssbo[stage][reg]; break;
            case SHB_N_UBOP: if (reg >= 0 && reg < 16) u = &S.ubo[stage][reg]; break;
            default: break;   // drvcb / 未解出的槽：全零段（出包静态统计证明驱动段没人读）| drvcb and unresolved slots: the zero bank (packaging proved nobody reads the driver segments)
            }
            uint32_t ao = 0;
            if (u && u->addr && arena_put(u->addr, u->size, 1024, &ao)) { buf = g_arena; off = ao; range = (u->size ? u->size : 1) + 256; }
            if (range > lim) range = lim;
            bi[nb] = (VkDescriptorBufferInfo){ buf, off, range };
            wr[nw].pBufferInfo = &bi[nb++];
        }
        nw++;
    }
    vkUpdateDescriptorSets(g_dev, (uint32_t)nw, wr, 0, NULL);
    return 1;
}

static int sets_rebuild(int vi, int fi)
{
    VkDescriptorSet sets[2] = { NULL, NULL };
    VkDescriptorSetLayout lays[2] = { g_sho[vi].dsl, g_sho[fi].dsl };
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_dpool; ai.descriptorSetCount = 2; ai.pSetLayouts = lays;
    if (vkAllocateDescriptorSets(g_dev, &ai, sets) != VK_SUCCESS) { atomic_fetch_add(&g_set_oom, 1); return 0; }
    if (!set_fill(sets[0], vi, 0) || !set_fill(sets[1], fi, 1)) return 0;
    g_cur_set[0] = sets[0]; g_cur_set[1] = sets[1];
    g_cur_vi = vi; g_cur_fi = fi; g_sets_valid = 1;
    g_args_dirty[0] = g_args_dirty[1] = 0;
    // 每个程序键第一回建集时，把它两个阶段实际绑的贴图打全（进世界后地面整片纯绿：先看地形那笔到底采哪几张图、多大）
    // First descriptor build for each program key: print every texture the two stages bind, to see which images the terrain
    // draw samples now that the world's ground renders flat green.
    if (g_cur_prog) {
        static uint64_t seen[32][2]; static int nseen;
        uint64_t k0, k1; memcpy(&k0, g_cur_prog->key[0], 8); memcpy(&k1, g_cur_prog->key[0] + 8, 8);
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i][0] == k0 && seen[i][1] == k1) { known = 1; break; }
        if (!known && nseen < 32) {
            seen[nseen][0] = k0; seen[nseen][1] = k1; nseen++;
            static char line[1024];
            int q = snprintf(line, sizeof line, "[vk] 程序首绑 %016llx%016llx 绑定:", (unsigned long long)k0, (unsigned long long)k1);
            for (int st = 0; st < 2; st++) {
                int shown = 0;
                for (int t = 0; t < 128 && shown < 6; t++) {
                    uint64_t texh = S.tex[st][t];
                    if (!texh) continue;
                    uint64_t tid = texh & 0xfffff;   // 合并句柄低 20 位才是贴图号（与 set_fill 同一切法）| low 20 bits are the texture id (same split as set_fill)
                    vktex *tv = tex_of_tid(tid);
                    q += snprintf(line + q, sizeof line - (size_t)q, " %d:%d=t%llu", st, t, (unsigned long long)tid);
                    if (tv) q += snprintf(line + q, sizeof line - (size_t)q, "(%ux%u f%d u%d d%d b%d t%d r%llu m%d p%d L%llu)", tv->w, tv->h, (int)tv->fmt,
                                          tv->uploaded, tv->drawn, tv->pkg_bad, tv->pkg_tried, (unsigned long long)(tv->pkg_range >> 10),
                                          tv->d.mem != NULL, tv->d.pool != NULL, (unsigned long long)(tv->d.L.size >> 10));
                    else q += snprintf(line + q, sizeof line - (size_t)q, "(无图)");
                    shown++;
                    if (q > (int)sizeof line - 200) break;
                }
            }
            GVL("%{public}s", line);
        }
    }
    return 1;
}

// ---- 顶点流搬运：每条用到的流按 (地址, 长度) 搬进 arena | vertex streams: each used stream is copied once per (address, length) ----
static struct { uint64_t gen, addr, len; uint32_t off; int ok; } g_vtx[16];

static _Atomic uint64_t g_vap_patch;   // 134217728 那一刀真改了哪几条流（计数=命中的属性条数）

static int vtx_stream(int st, uint32_t *off)
{
    if (st < 0 || st > 15) return 0;
    uint64_t a = S.vbuf[st].address, l = S.vbuf[st].size;
    if (g_vtx[st].gen == g_arena_gen && g_vtx[st].addr == a && g_vtx[st].len == l) { *off = g_vtx[st].off; return g_vtx[st].ok; }
    uint32_t o = 0;
    int ok = a && l && arena_put(a, l, 4096, &o);
    // 位 134217728（B46 探针）：把「四分量 8 位」顶点属性（RGBA8 那一族）的第 4 字节一律顶成 0xFF。
    // 和上面的 va_alpha_said 配成一对：普查说"是不是 0"，这一刀说"把它当 1 用画面会怎样"。
    // 只是探针，不是修法 —— 真修法要等普查点名是哪一条流、哪个偏移。
    if (ok && (g_skip & 134217728)) {
        uint32_t stride = S.stream[st].stride;
        if (stride >= 4)
            for (int loc = 0; loc < S.nattr && loc < 16; loc++) {
                int f = (int)S.attr[loc].fmt;
                if (f < 0x01 || f > 0x30 || 1 + (f - 1) / 12 != 4 || (f - 1) % 12 > 3) continue;
                if ((int)S.attr[loc].stream != st) continue;
                for (uint32_t p = S.attr[loc].offset + 3; p < l; p += stride) g_arena_map[o + p] = 0xFF;
                atomic_fetch_add(&g_vap_patch, 1);
            }
    }
    g_vtx[st].gen = g_arena_gen; g_vtx[st].addr = a; g_vtx[st].len = l; g_vtx[st].off = o; g_vtx[st].ok = ok;
    *off = o;
    return ok;
}

// ---- 每次提交的重放记账 | per-submission replay bookkeeping ----
static int g_draw_ready, g_draw_fail;
static _Atomic uint64_t g_dr_replay, g_dr_nolib, g_dr_bad, g_dr_nopipe, g_dr_over, g_dr_msaa, g_dr_nopass, g_vp_neg;
static int g_dr_thissub, g_draw_max = 2000;
static VkPipeline g_last_pipe;
static VkPipelineLayout g_last_pl;

static int draw_boot(void)
{
    if (g_draw_ready) return 1;
    if (g_draw_fail) return 0;
    if (!shb_load()) { g_draw_fail = 1; return 0; }
    const char *dm = getenv("DM_VK_DRAW_MAX");
    if (dm && *dm) { int v = (int)strtol(dm, NULL, 0); if (v > 0) g_draw_max = v; }
    if (!dummy_boot()) { GRED("哑资源没建起来：不重放绘制"); g_draw_fail = 1; return 0; }
    if (!mk_hostbuf(ARENA_SZ, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                          | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                          | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    &g_arena, &g_arena_mem, (void **)&g_arena_map)) { GRED("arena 没建起来：不重放绘制"); g_draw_fail = 1; return 0; }
    memset(g_arena_map + (ARENA_SZ - ARENA_GUARD), 0, ARENA_GUARD);
    {
        VkDescriptorPoolSize ps[4] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12000 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12000 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 } };
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = (uint32_t)(2 * g_draw_max); pi.poolSizeCount = 4; pi.pPoolSizes = ps;
        if (vkCreateDescriptorPool(g_dev, &pi, NULL, &g_dpool) != VK_SUCCESS) { GRED("建描述符池失败（组 %{public}d）", pi.maxSets); g_draw_fail = 1; return 0; }
    }
    state_reset();
    GVL("[vk] 绘制重放就绪：arena %{public}llu MB、一次提交上限 %{public}d 绘、着色器库 %{public}d 条",
        (unsigned long long)(ARENA_SZ >> 20), g_draw_max, g_shb_n);
    g_draw_ready = 1;
    return 1;
}

void mtl_sub_reset_draw(void);   // 由 mtl_sub_begin 调：换提交就清空 arena / 描述符 / 顶点流缓存 | called from mtl_sub_begin: a new submission starts with an empty arena, sets and stream cache
void mtl_sub_reset_bigstage(void);   // 巨图暂存游标与 arena 同步清零（定义在打包上传那一段）| the big staging cursor resets with the arena
void mtl_sub_reset_draw(void)
{
    g_arena_gen++;
    g_arena_off = 0;
    mtl_sub_reset_bigstage();
    g_dr_thissub = 0;
    g_sets_valid = 0; g_cur_vi = g_cur_fi = -1;
    g_last_pipe = NULL; g_last_pl = NULL;
    args_dirty_all();
    if (g_dpool) vkResetDescriptorPool(g_dev, g_dpool, 0);
}

static void emit_dyn(VkCommandBuffer cb, VkExtent2D ext)
{
    VkViewport v = { 0, 0, (float)ext.width, (float)ext.height, S.dr[0], S.dr[1] };
    if (S.has_vp) { v.x = S.vp[0][0]; v.y = S.vp[0][1]; v.width = S.vp[0][2]; v.height = S.vp[0][3]; }
    // Vulkan 1.0 规定视口偏移非负（VUID-VkViewport-x/y-01231/2），GL 习惯的负 y 界面矩形在这里会出现；先原样传、只计数，
    // 画面真反了再一次性按 NVN→Vk 的惯例改 y 翻转。
    // Vulkan 1.0 wants non-negative viewport offsets; GL-style negative y does show up here. Passed through as-is with a counter for now -
    // if the picture comes out flipped this becomes one deliberate y-flip change.
    if (v.x < 0 || v.y < 0) g_vp_neg++;
    // 视口 Y 翻：默认开。第 112 轮 B36 用运行时位 2097152 单独验过——饥荒画的是 NVN 的左下原点，
    // 我们原样喂给 Vulkan（左上原点）就等于整幅上下翻一遍；开了之后截图里「开始游戏！」是正的，
    // 而同版二进制不开就是镜像（B35）。出图那颗满屏三角形（tri.vert 的 uv=q*0.5+0.5）本身也翻一次，
    // 两次翻合起来才是正的，所以**别**再去改出图那侧。负高度视口靠 VK_KHR_maintenance1（本机在场）。
    // 位 8388608 = 关掉这一翻，留着做反向 A/B。
    // Viewport Y flip, on by default. Round 112 (B36) proved it with the runtime bit: Don't Starve writes NVN's
    //   lower-left-origin viewports, Vulkan's origin is upper-left, so passing it through flips the whole picture.
    //   With the flip the screenshot reads upright; without it (B35, same binary) it is a vertical mirror.
    //   The present triangle flips once more on its own (uv = q*0.5+0.5), which is why the blit side must NOT change.
    //   Bit 8388608 turns this off for the reverse A/B. Negative viewport height needs VK_KHR_maintenance1 (present here).
    int yflip = !(g_skip & 8388608);
    if (yflip) { v.y += v.height; v.height = -v.height; }
    vkCmdSetViewport(cb, 0, 1, &v);
    VkRect2D sc = { { 0, 0 }, ext };
    if (S.has_sc) {
        sc.offset.x = S.sc.x;
        sc.extent.width = (uint32_t)(S.sc.w < 0 ? 0 : S.sc.w);
        sc.extent.height = (uint32_t)(S.sc.h < 0 ? 0 : S.sc.h);
        // 裁剪盒跟着视口翻：不然几何翻正了、可见矩形还留在镜像的那一排（下沿越界就夹成 0，整块 UI 会凭空消失）
        // The scissor rides along with the viewport, otherwise the geometry is upright but the visible rect stays
        // on the mirrored row (and an out-of-range origin clips a whole panel away).
        int sy = yflip ? (int)ext.height - (int)S.sc.y - (int)sc.extent.height : (int)S.sc.y;
        sc.offset.y = sy < 0 ? 0 : sy;
    }
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthBias(cb, S.po.units, S.po.clamp, S.po.factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_vmask[0]);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_vmask[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_wmask[0]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_wmask[1]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_ref[0]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, S.st_ref[1]);
    vkCmdSetLineWidth(cb, 1.0f);
}

// ---- OP_COPY_B2T：把游戏内存里的像素搬进 VkImage（M2-3c）| pushing the game's own texels into the VkImage ----
// 参照实现是 nvn_mtl.m:4667 的 copy_b2t：行距取 S.copy_stride（没设就按块宽数 × 每块字节算），层从 view 的 min_layer 起，
// 每个 z 一张 2D 图（3D 图一次拷 nz 层）。压缩格式（BC / ASTC）按 4×4 / n×m 块计算偏移与 extent，前提是设备认这个块格式（probe_bc）。

// ---- BC → 未压缩像素的 CPU 解码复用 dm/gfx/tex/nvn_bcdec.c：Maleoon 920 实测一个 BC 块都不认（第 81 轮），BC1–BC7 就按块解开再上传 ----
#include "../tex/nvn_bcdec.h"
#include "../tex/nvn_deswizzle.h"   // 打包贴图的解排布（M2-3f）| deswizzle for packaged textures (M2-3f)

static _Atomic uint64_t g_b2t_ok, g_b2t_noaddr, g_b2t_inpass, g_b2t_comp, g_b2t_dst, g_b2t_soft;

static uint32_t arena_take(uint64_t len)   // 只要一段对齐的空位，内容自己写（解码上传用）| reserve aligned space; caller fills it
{
    if (!g_arena_map || !len) return 0xFFFFFFFFu;
    uint64_t need = (len + 15) & ~(uint64_t)15;
    uint64_t o = (g_arena_off + 15) & ~(uint64_t)15;
    if (o + need > ARENA_SZ - ARENA_GUARD) { atomic_fetch_add(&g_arena_full, 1); return 0xFFFFFFFFu; }
    g_arena_off = o + need;
    return (uint32_t)o;
}

static uint32_t arena_raw(uint64_t addr, uint64_t len)
{
    void *base; uint64_t bo;
    if (!addr || !len) return 0xFFFFFFFFu;
    if (!mtl_addr(addr, len, &base, &bo)) { atomic_fetch_add(&g_b2t_noaddr, 1); return 0xFFFFFFFFu; }
    uint64_t need = (len + 15) & ~(uint64_t)15;
    uint64_t o = (g_arena_off + 15) & ~(uint64_t)15;   // 拷贝的缓冲偏移要 16 对齐：arena_put 的记账会把游标弄歪，这里自己夹回来
    if (o + need > ARENA_SZ - ARENA_GUARD) { atomic_fetch_add(&g_arena_full, 1); return 0xFFFFFFFFu; }
    g_arena_off = o + need;
    memcpy(g_arena_map + o, (uint8_t *)base + bo, len);
    return (uint32_t)o;
}

// 上传前验一眼 CPU 侧像素：第 90 轮出图源读回 RGB 恒 0、alpha 却写了 34%，说明画了但采到的是空纹理。
// 第 91 轮实测「软解出来的像素就是全零」，所以要把范围再夹一层：源字节（游戏那块缓冲）也探一次。
// 源有内容、解出来全零 ⇒ 解码器/块尺寸/行距错；源也全零 ⇒ 取源地址错，或游戏根本没往这块写过像素。
// 采样用 xorshift 而不是等距：等距步长正好是 4 的倍数时会只踩到某一个通道，第一版就被它骗过一次。
// Probe the CPU-side pixels before the upload. Round 90's readback showed the present source with RGB exactly zero while alpha was
// written, so the shaders drew onto empty textures. Round 91 measured the soft-decoded pixels as already zero, so probe the guest
// source bytes too: source has content but the decode is zero = decoder/block-size/stride bug; source is zero too = wrong address
// or the game never wrote pixels there. Sampling walks by xorshift, not evenly - an even step that lands on a multiple of four only
// reads one channel, which fooled the first version of this probe.
static int g_up_probe_n;
static void up_probe(const char *tag, const uint8_t *p, uint64_t n, uint32_t w, uint32_t h, int tally)
{
    if (!n || !p) return;
    uint64_t mx = 0, nz = 0, x = 88172645463325252ull;
    for (int k = 0; k < 1024; k++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint8_t b = p[x % n];
        if (b > mx) mx = b;
        if (b) nz++;
    }
    if (tally) { if (nz) atomic_fetch_add(&g_up_content, 1); else atomic_fetch_add(&g_up_zero, 1); }
    if (g_up_probe_n >= 16) return;
    char rb[176];
    snprintf(rb, sizeof rb, "[vk] 上传探针 %s %ux%u 字节%llu 采样1024 非零%llu 最大%llu", tag, w, h,
             (unsigned long long)n, (unsigned long long)nz, (unsigned long long)mx);
    GVL("%{public}s", rb);
    g_up_probe_n++;
}

static void do_copy_b2t(const p_copy_b2t *c)
{
    if (!c->tex || !c->tex->serial || !c->r.width || !c->r.height) return;
    if (!g_draw_ready && !draw_boot()) return;   // arena 是 draw_boot 才建的：上传不能跑在它前面
    if (!g_arena_map) return;
    if (g_in_pass || !g_cb) { atomic_fetch_add(&g_b2t_inpass, 1); return; }   // 通道开着发不了 transfer：这刀先记账
    vktex *v = tex_get(c->tex);
    if (!v || !v->img || v->is_depth) {
        atomic_fetch_add(&g_b2t_dst, 1);
        static int saidb;
        if (saidb < 4) {
            saidb++;
            GVL("[vk] B2T败 s=%{public}llu f=%{public}d %{public}u x %{public}u t=%{public}d 区=%{public}d x %{public}d 有图%{public}d 深度%{public}d 层%{public}u mip%{public}u",
                (unsigned long long)c->tex->serial, c->tex->d.format, c->tex->d.width, c->tex->d.height, c->tex->d.target,
                c->r.width, c->r.height, v ? (v->img != NULL) : 0, v ? v->is_depth : -1,
                c->tex->L.layers, c->tex->d.levels);
        }
        return;
    }
    int bw = 1, bh = 1;
    int bpb = nvn_fmt_bpb(v->d.d.format, &bw, &bh);
    if (!bpb) { atomic_fetch_add(&g_b2t_comp, 1); return; }   // 认不出的格式：等 CPU 解码那一刀
    unsigned level = (c->has_view && c->view.set_levels) ? (unsigned)c->view.base_level : 0u;
    unsigned layer0 = (c->has_view && c->view.set_layers) ? (unsigned)c->view.min_layer : 0u;
    if (level >= v->levels) return;
    int nz = c->r.depth > 1 ? c->r.depth : 1;
    if (v->soft_bc) {
        // BC 块 → 未压缩像素：源按 4×4 块寻址，解出来的像素写进 arena 再拷图（设备不认 BC，第 81 轮实测）。
        // 解码器复用 dm/gfx/tex/nvn_bcdec.c（BC1–BC7 / BC6H，Metal 后端一直在用），目标格式与它给的每像素字节数一致。
        uint32_t pf; int bpp = 4;
        if (!nvn_bc_unpacked((uint32_t)v->d.d.format, &pf, &bpp)) { atomic_fetch_add(&g_b2t_comp, 1); return; }
        uint32_t nbx = (uint32_t)((c->r.width + bw - 1) / bw), nby = (uint32_t)((c->r.height + bh - 1) / bh);
        uint64_t sp = (S.copy_stride > 0 && ((uint64_t)S.copy_stride % (uint64_t)bpb) == 0) ? (uint64_t)S.copy_stride : (uint64_t)nbx * bpb;
        uint64_t s_img = sp * nby;
        uint32_t dw = (uint32_t)c->r.width, dh = (uint32_t)c->r.height;
        uint32_t lw = (v->w >> level) ? (v->w >> level) : 1u, lh = (v->h >> level) ? (v->h >> level) : 1u;
        if (dw > lw) dw = lw;                    // 目标要夹到这一 mip 级的尺寸：mip 越往下，越界写会踩到 arena 之外的图
        if (dh > lh) dh = lh;
        uint64_t d_row = (((uint64_t)dw * (uint64_t)bpp) + 15) & ~(uint64_t)15;
        uint64_t d_img = d_row * (uint64_t)dh;
        void *sb = NULL; uint64_t sbo = 0;
        if (!mtl_addr(c->src, s_img * (uint64_t)nz, &sb, &sbo)) { atomic_fetch_add(&g_b2t_noaddr, 1); return; }
        up_probe("源BC", (const uint8_t *)sb + sbo, s_img * (uint64_t)nz, dw, dh, 0);
        uint32_t ao = arena_take(d_img * (uint64_t)nz);
        if (ao == 0xFFFFFFFFu) return;
        for (int z = 0; z < nz; z++) {
            if (nvn_bc_decode((uint32_t)v->d.d.format, (const uint8_t *)sb + sbo + (uint64_t)z * s_img, sp,
                              dw, dh, g_arena_map + ao + (uint64_t)z * d_img, (size_t)d_row)) {
                atomic_fetch_add(&g_b2t_comp, 1);
                return;
            }
        }
        to_layout(g_cb, v, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        up_probe("软解", g_arena_map + ao, d_img * (uint64_t)nz, dw, dh, 1);
        VkBufferImageCopy reg[16]; int nreg = 0;
        for (int z = 0; z < nz && nreg < 16; z++) {
            uint32_t layer = layer0 + (uint32_t)(c->r.zoffset + z);
            if (v->d.d.target == 2) layer = 0;
            reg[nreg++] = (VkBufferImageCopy){
                .bufferOffset = ao + (uint64_t)z * d_img,
                .bufferRowLength = (uint32_t)(d_row / (uint64_t)bpp),
                .bufferImageHeight = dh,
                .imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 },
                .imageOffset = { c->r.xoffset, c->r.yoffset, v->d.d.target == 2 ? c->r.zoffset : 0 },
                .imageExtent = { dw, dh, v->d.d.target == 2 ? nz : 1u } };
            if (v->d.d.target != 2) continue;
            break;
        }
        vkCmdCopyBufferToImage(g_cb, g_arena, v->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)nreg, reg);
        v->uploaded = 1;
        // 传完就转可采样：游戏的用法是「上传完下一帧就当贴图采」，留在 TRANSFER_DST 会让绑定时布局不对
        // (第 85 轮实测：1131 次真图绑定，布局不对 1131 次，一次都没对上)。
        // Move to sampleable right after the upload: the game samples what it just uploaded, and leaving it in TRANSFER_DST made every
        // real binding read a wrong layout (round 85: 1131 real binds, 1131 of them with the layout not yet set).
        to_layout(g_cb, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        atomic_fetch_add(&g_b2t_ok, 1); atomic_fetch_add(&g_b2t_soft, 1);
        static int saids;
        if (saids < 3) {
            saids++;
            GVL("[vk] B2T软解 s=%{public}llu NVN%{public}d → %{public}d bpp %{public}d %{public}u x %{public}u 层%{public}d 源行距%{public}llu",
                (unsigned long long)c->tex->serial, v->d.d.format, v->fmt, bpp, dw, dh, nz, (unsigned long long)sp);
        }
        return;
    }
    if (bw != 1 || bh != 1) {
        // 压缩块：VkImageCopy 的偏移与尺寸必须整块对齐，游戏里 B2T 基本都是整图/整 mip，不整齐的这刀先放过
        if (c->r.xoffset % bw || c->r.yoffset % bh) { atomic_fetch_add(&g_b2t_comp, 1); return; }
    }
    // 块格式按「块数」算，块数还要夹到该 mip 级应有的块数
    uint32_t ebw = (uint32_t)((c->r.width + bw - 1) / bw);
    uint32_t ebh = (uint32_t)((c->r.height + bh - 1) / bh);
    uint32_t lw = (v->w >> level) ? (v->w >> level) : 1u;
    uint32_t lh = (v->h >> level) ? (v->h >> level) : 1u;
    uint32_t mbw = (lw + (uint32_t)bw - 1) / (uint32_t)bw;
    uint32_t mbh = (lh + (uint32_t)bh - 1) / (uint32_t)bh;
    if (ebw > mbw) ebw = mbw;
    if (ebh > mbh) ebh = mbh;
    if (!ebw || !ebh) return;
    uint64_t st = (uint64_t)S.copy_stride;
    uint64_t row = (S.copy_stride > 0 && (st % (uint64_t)bpb) == 0) ? st : (uint64_t)ebw * (uint64_t)bpb;
    uint64_t img = row * ebh;
    uint32_t ao = arena_raw(c->src, img * (uint64_t)nz);
    if (ao == 0xFFFFFFFFu) return;
    up_probe("直传", g_arena_map + ao, img * (uint64_t)nz, ebw * (uint32_t)bw, ebh * (uint32_t)bh, 1);
    to_layout(g_cb, v, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy reg[16]; int nreg = 0;
    for (int z = 0; z < nz && nreg < 16; z++) {
        uint32_t layer = layer0 + (uint32_t)(c->r.zoffset + z);
        if (v->d.d.target == 2) layer = 0;   // 3D 图：一次覆盖 nz 层，zoffset 走 imageOffset.z
        reg[nreg++] = (VkBufferImageCopy){
            .bufferOffset = ao + (uint64_t)z * img,
            .bufferRowLength = (uint32_t)(row / bpb) * (uint32_t)bw,
            .bufferImageHeight = (uint32_t)(img / row) * (uint32_t)bh,
            .imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 },
            .imageOffset = { c->r.xoffset, c->r.yoffset, v->d.d.target == 2 ? c->r.zoffset : 0 },
            .imageExtent = { ebw * (uint32_t)bw, ebh * (uint32_t)bh, v->d.d.target == 2 ? nz : 1u } };
        if (v->d.d.target != 2) continue;
        break;   // 3D：一条区域就够
    }
    vkCmdCopyBufferToImage(g_cb, g_arena, v->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)nreg, reg);
    v->uploaded = 1;
    to_layout(g_cb, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // 同上：传完即可采 | same as above: ready to sample once uploaded
    atomic_fetch_add(&g_b2t_ok, 1);
    static int saido;
    if (saido < 3) {
        saido++;
        GVL("[vk] B2T成 s=%{public}llu f=%{public}d %{public}u x %{public}u 区 %{public}d x %{public}d 起 %{public}d,%{public}d,%{public}d lvl %{public}u lay %{public}u 行距 %{public}llu nz %{public}d",
            (unsigned long long)c->tex->serial, c->tex->d.format, c->tex->d.width, c->tex->d.height,
            c->r.width, c->r.height, c->r.xoffset, c->r.yoffset, c->r.zoffset, level, layer0,
            (unsigned long long)row, nz);
    }
}

// 贴图两级落盘（DM_TEXDUMP = 绝对输出目录，例：/data/storage/el2/base/haps/entry/files/tex）：
// A = 解完排布的**压缩块**，B = 我们 CPU 软解出来的 RGBA。
// 第 113 轮 B37 的现场：版面、文字、位置全对，但 UI 面板被平涂的横条纹占掉。这既可能是「排布/行距算错」，
// 也可能是「BC 解压错」，光看屏幕分不开。同一张图两份都落盘，离线把 A 当 DDS 用已知正确的解码器解一遍和 B 对：
// A 正 B 歪 = 我们的解压错；A 就歪 = 排布/几何错。一枪分辨两半，别猜。
// Two-stage texture dump: A = deswizzled compressed blocks, B = our CPU-decoded RGBA. The banded panels can come from
// either stage, so both are written for the same serial; decoding A offline with a known-good BC decoder says which half is wrong.
// 开关的值本身就是目录（第 114 轮 B38：值写 1、落盘到 DM_SAVE 时 16 行日志全打了、目录里一个文件都没有，
// 而 DM_VK_KEYDUMP 往 files/ 写 ctl.bin 是成的 ⇒ 存档目录那条路不能当输出用，失败必须当场报 errno 并停手）。
static void tex_dump(uint64_t serial, unsigned lvl, unsigned nlv, const void *a, size_t an, const void *b, size_t bn,
                     uint32_t w, uint32_t h, uint32_t rb, int nvnfmt, int vkfmt, int soft)
{
    static int n; static size_t tot;
    const char *d = getenv("DM_TEXDUMP");
    // 张数与总量双闸：一张带 mip 的要占两格（级 0 + 级 1），40 格 ≈ 20 张，够把开局那批 UI 图集全带出来。
    // Slots are per level, so 40 covers roughly 20 mipmapped textures - the boot-time UI atlases are the ones to compare.
    if (!d || d[0] == '0' || n >= 40 || tot > (128u << 20)) return;
    if (d[0] != '/') { GVL("[vk] 贴图落盘没开：DM_TEXDUMP 要给绝对目录，现在是 %{public}s", d); return; }
    if (an > (8u << 20) || bn > (8u << 20)) return;   // 单张 8 MB 封顶
    static int dir_made;
    if (!dir_made) { dir_made = 1; if (mkdir(d, 0755) && errno != EEXIST) GVL("[vk] 贴图落盘目录建不出来 %{public}s errno=%{public}d", d, errno); }
    n++; tot += an + bn;
    char p[512];
    FILE *f;
    snprintf(p, sizeof p, "%s/tex%u_s%llu_L%u_A.bin", d, n, (unsigned long long)serial, lvl);
    f = fopen(p, "wb");
    if (f) { fwrite(a, 1, an, f); fclose(f); }
    else { GVL("[vk] 贴图落盘写不出 %{public}s errno=%{public}d（停手）", p, errno); n = 40; return; }
    if (b && bn) {
        snprintf(p, sizeof p, "%s/tex%u_s%llu_L%u_B.bin", d, n, (unsigned long long)serial, lvl);
        f = fopen(p, "wb"); if (f) { fwrite(b, 1, bn, f); fclose(f); }
    }
    GVL("[vk] 贴图落盘%{public}u serial=%{public}llu 级%{public}u 共%{public}u NVN%{public}d→Vk%{public}d 软%{public}d %{public}u x %{public}u 块行%{public}u A=%{public}zu B=%{public}zu",
        n, (unsigned long long)serial, lvl, nlv, nvnfmt, vkfmt, soft, w, h, rb, an, bn);
}

// 整张块线性存储原样落盘一份（raw_s<serial>.bin）+ 把描述符、本层算出的总大小/每级起点/每级块高块深打进日志。
// 为什么要整张：级 ≥1 的几何（每级起点、tileH 收缩）一旦有嫌疑，逐个假设上真机试就是一轮一刀，太慢；
// 整张取一次样本，离线把「tileH 各档 × 起点各算法」全穷举一遍，哪档能解出连续美术，答案就是它。
// Dump the whole block-linear image once, plus the descriptor and our per-level offsets/tiling: the per-level layout
// hypotheses then get brute-forced offline instead of costing one device round each.
static void tex_dump_raw(const nvn_texdesc *d, uint64_t serial, const void *mem, size_t avail)
{
    static int n; static size_t tot;
    const char *dir = getenv("DM_TEXDUMP");
    if (!dir || dir[0] == '0' || dir[0] != '/' || n >= 8 || tot > (64u << 20)) return;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    size_t sz = L.size < (uint64_t)avail ? (size_t)L.size : avail;
    if (!sz || sz > (8u << 20)) return;                 // 单张 8 MB 封顶，8 张 64 MB 封顶
    char p[512];
    FILE *f = fopen((snprintf(p, sizeof p, "%s/raw_s%llu.bin", dir, (unsigned long long)serial), p), "wb");
    if (!f) { GVL("[vk] 整张贴落盘写不出 %{public}s errno=%{public}d（停手）", p, errno); n = 8; return; }
    fwrite(mem, 1, sz, f); fclose(f);
    n++; tot += sz;
    int bw = 1, bh = 1; uint32_t bp2 = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    unsigned lv = (unsigned)(d->levels ? d->levels : 1);
    char lo[512]; int k = 0;
    for (unsigned l = 0; l < lv && l < 10; l++) {
        unsigned th, td; nvn_tex_level_tiles(d, l, &th, &td);
        k += snprintf(lo + k, k < (int)sizeof lo ? (size_t)sizeof lo - (size_t)k : 0, "%s%llu:%u/%u",
                      l ? " " : "", (unsigned long long)nvn_tex_level_offset(d, l), th, td);
    }
    GVL("[vk] 整张贴%{public}u serial=%{public}llu 目标%{public}d 格式%{public}d 标志%{public}d 采样%{public}d %{public}u x %{public}u x %{public}u 级%{public}u 跨距%{public}u 包块高%{public}d｜字节%{public}zu 每层%{public}llu 对齐%{public}u 层%{public}u 块%{public}dx%{public}d 块字%{public}u 整块高%{public}u 整块深%{public}u｜各级 起点:块高:块深=%{public}s",
        n, (unsigned long long)serial, d->target, d->format, d->flags, d->samples, d->width, d->height, d->depth,
        d->levels, d->stride, d->pkg_block_h, sz, (unsigned long long)L.layer_size, L.align, L.layers, bw, bh, bp2,
        L.tileH, L.tileD, lo);
}

// ---- M2-3f 打包贴图上传：游戏内存池里的块线性像素 → 解排布 →（BC 再 CPU 解压）→ arena → VkImage ----
// 第 109 轮实测：几张「图建好了却没有任何来源」的池内存各采样 8 KB 就有 1000–7000 个非零字节（serial 181 272x228、
// serial 177 7992x164……全是 RGBA8 界面图集）⇒ 像素一直在客体内存里躺着，Vulkan 这一侧从没搬过：B2T 那条路只覆盖
// 游戏显式提交的拷贝命令，卡带里预烘的贴图靠的是「登记时带 nvn_texlayout + 指向池内存储」这一路，参照 nvn_mtl.m:1089。
// 第 110 轮实测修正：这一路只搬成 8 张、不到 1 MB，而哑绑定还有 209 万次 ⇒ 打包贴图确实缺，但**不是平涂白的主因**。
// 下一轮的判据在 EMP 那行（见 tex_has_content 下面）：按绑定次数排出前 6 张空图，看它们有没有池内容。
// Round 109 measured: several "image built but no source" textures held 1000-7000 non-zero bytes per 8 KiB pool sample, so the
// pixels sat in guest memory unmoved - B2T only covers explicit copy commands, while baked-in assets come through "registered
// with an nvn_texlayout pointing at pool storage" (nvn_mtl.m:1089). Round 110 corrected the picture: this path moved 8 images /
// under 1 MB while dummy binds were still 2.09M, so packaged textures are a real gap but NOT the cause of the flat white frame.
// The next round's verdict lives in the EMP line (see below tex_has_content): the top six empty images by bind count.
static _Atomic uint64_t g_pkg_up, g_pkg_bytes, g_pkg_arena, g_pkg_range, g_pkg_dz, g_pkg_fmt, g_pkg_3d,
                        g_pkg_bc, g_pkg_dep, g_pkg_big, g_pkg_bigok, g_pkg_q, g_pkg_re;
#define PKG_PEND 512   // 加了重传之后排队的不只新建的图，一帧里被游戏改写的那几张也要挤进同一批，256 会饿住新图
static vktex *g_pkg_pend[PKG_PEND];
static int g_pkg_pend_n;

// 巨图暂存缓冲：arena 只有 64 MB，还要留给顶点流与 BufferToTexture，所以超过 ARENA_SZ/3 的打包贴图整张被拒
// （第 111 轮「太大3」= s311 2048x2048 NVN 0x37，池里 8 KB 采样有 4164 个非零字节，像素确实在）。
// 这一块按「整张一次圈够」用：圈不到就整张不搬，绝不留下半张贴图（游标与 arena 一样每次提交清零——
// 提交末尾等完栅栏，上一批拷贝早已跑完，所以同一提交内不复用、不折叠）。首次需要时才建，平时不占这 160 MB。
// Big staging buffer: the 64 MB arena must also serve the vertex stream and BufferToTexture, so packaged textures above
// ARENA_SZ/3 were refused whole (round 111's "太大3" = s311 2048x2048 NVN 0x37, whose pool bytes demonstrably hold content).
// One texture reserves its entire footprint here or moves nothing - never half an image. The cursor resets with the arena per
// submission, which is safe because mtl_sub_commit waits its fence before the next submission starts. Built lazily on first need.
#define BIG_STAGE_SZ (160ull << 20)
#define ARENA_PKG_CAP (ARENA_SZ / 3)
static VkBuffer g_bigs; static VkDeviceMemory g_bigs_mem; static uint8_t *g_bigs_map;
static uint64_t g_bigs_off;
static _Atomic int g_birs_bad;   // 建这块失败过就别每次重试 | once the allocation failed, do not retry every texture

typedef struct { VkBuffer buf; uint8_t *map; uint64_t off, cap; int big; } pkg_stage;

static int bigs_build(void)
{
    if (g_bigs_map) return 1;
    if (atomic_load(&g_birs_bad)) return 0;
    if (!mk_hostbuf(BIG_STAGE_SZ, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_bigs, &g_bigs_mem, (void **)&g_bigs_map)) {
        atomic_store(&g_birs_bad, 1); GRED("巨图暂存 %{public}llu MB 建不起来，超过 arena 的打包贴图只能继续拒",
                                           (unsigned long long)(BIG_STAGE_SZ >> 20));
        return 0;
    }
    GVL("[vk] 巨图暂存就绪：%{public}llu MB（arena 的 %{public}llu MB 留给顶点流）",
        (unsigned long long)(BIG_STAGE_SZ >> 20), (unsigned long long)(ARENA_SZ >> 20));
    return 1;
}

// 为一张图圈暂存：能进 arena 就用 arena（游标共享、逻辑与原来一致），否则走巨图块
// Reserve staging for one image: the arena when it fits (shared cursor, exactly the old behaviour), the big buffer otherwise.
static int stage_begin(pkg_stage *s, uint64_t need)
{
    s->off = 0;
    if (need <= ARENA_PKG_CAP) { s->buf = g_arena; s->map = g_arena_map; s->cap = ARENA_PKG_CAP; s->big = 0; return 1; }
    if (!bigs_build()) return 0;
    s->buf = g_bigs; s->map = g_bigs_map; s->cap = BIG_STAGE_SZ; s->big = 1;
    if (g_bigs_off + need > s->cap) return 0;   // 本次提交里巨图块满了：下次提交（游标清零）再来 | full for this submission: retry after the cursor resets
    s->off = g_bigs_off;
    return 1;
}
static uint64_t stage_take(pkg_stage *s, uint64_t len)   // 返回 UINT64_MAX = 没圈到 | UINT64_MAX = nothing left
{
    uint64_t need = (len + 15) & ~(uint64_t)15;
    if (s->big) {
        if (s->off + need > s->cap) return ~(uint64_t)0;
        uint64_t o = s->off; s->off += need; g_bigs_off = s->off;
        return o;
    }
    uint32_t a = arena_take(len);
    return a == 0xFFFFFFFFu ? ~(uint64_t)0 : (uint64_t)a;
}
void mtl_sub_reset_bigstage(void);
void mtl_sub_reset_bigstage(void) { g_bigs_off = 0; }

static void pkg_big_said(const vktex *v, uint64_t tot, uint64_t cmax)
{
    static int said;
    if (said >= 8) return;
    said++;
    GVL("[vk] 太大%{public}d s=%{public}llu NVN%{public}d %{public}u x %{public}u 层%{public}u 级%{public}u 软解%{public}d 要%{public}llu MB arena上限%{public}llu MB",
        said, (unsigned long long)v->serial, (int)v->d.d.format, v->w, v->h, v->layers, v->levels, v->soft_bc,
        (unsigned long long)((tot + cmax) >> 20), (unsigned long long)(ARENA_PKG_CAP >> 20));
}

static void pkg_upload(vktex *v)
{
    const tex_t *t = &v->d;
    if (!g_cb || g_in_pass || !v->img) return;   // 通道里发不了 transfer，也不是失败：留着下次冲刷
    if (!v->pkg_tried) { v->pkg_tried = 1; atomic_fetch_add(&g_pkg_q, 1); }
    // has_layout 不再当闸门（第 109 轮实测）：饥荒一张图都没调 TextureBuilderSetLayout ⇒ EMP pk=0 nopk=53676，
    // 可像素确实躺在池里（8 KB 采样非零 4164~7659，m1）。这条路的排布每步都按描述符现算（nvn_deswizzle_geom /
    // nvn_deswizzle），has_layout 只说明「打包贴图自带布局描述」，用它把关等于把上传整条锁在没人用的那半边。
    if (!t->mem || !t->pool) { v->pkg_bad = 1; return; }
    if (v->is_depth || t->d.target == 2) {   // 深度没有池内容；3D 在本层按深度拆成数组层，形状对不上，这一刀放过
        atomic_fetch_add(t->d.target == 2 ? &g_pkg_3d : &g_pkg_dep, 1);
        v->pkg_bad = 1; return;
    }
    int bw = 1, bh = 1;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    int soft = v->soft_bc;                        // 马良 920 一个 BC 块格式都不认（第 81 轮实测）：块先解排布，再在 CPU 上解压
    int bpp = bpb; uint32_t upf = 0;
    if (soft) { if (!nvn_bc_unpacked((uint32_t)t->d.format, &upf, &bpp)) { atomic_fetch_add(&g_pkg_fmt, 1); v->pkg_bad = 1; return; } }
    else if (!bpb || bw != 1 || bh != 1) { atomic_fetch_add(&g_pkg_fmt, 1); v->pkg_bad = 1; return; }
    uint64_t s0 = t->off > 0 ? (uint64_t)t->off : 0;
    uint64_t avail = t->pool->size > s0 ? t->pool->size - s0 : 0;
    if (!avail) { v->pkg_bad = 1; return; }
    int lv = (int)v->levels, la_n = (int)v->layers;
    // 先量一遍再动手：一张图要么整张搬完，要么别搬。arena 是每条提交才清的 bump 分配器，中途只剩一半会留下半张贴图。
    // Measure first: an image either moves whole or not at all. The arena is a per-submission bump allocator, so running out
    // halfway would leave a half-filled texture behind.
    uint64_t tot = 0, cmax = 0, pend = 0;
    uint64_t st0 = nvn_dirty_now();   // 戳要在读像素之前取：读期间游戏若写了这页，代号必 > 本戳 ⇒ 下一次用就重传
    for (int l = 0; l < lv; l++) for (int la = 0; la < la_n; la++) {
        uint32_t rb, rows, slices, bp2; uint64_t bs; unsigned th, td;
        if (nvn_deswizzle_geom(&t->d, (unsigned)l, (unsigned)la, &rb, &rows, &slices, &bs, &th, &td, &bp2) || !rb) {
            atomic_fetch_add(&g_pkg_dz, 1); v->pkg_bad = 1; return;
        }
        uint64_t need = (uint64_t)rb * rows * slices;
        // 池尾之外就是又一次 SIGSEGV：解排布按 16 字节段跳着读，越界不会连续踩，靠探针抓不到，只能在这里夹死
        // Reading past the end of the pool is another SIGSEGV, and the deswizzle walks in 16-byte jumps so a stray read is not
        // contiguous - the only safe place to clamp is here.
        if (bs + need > avail) { atomic_fetch_add(&g_pkg_range, 1); v->pkg_bad = 1; return; }
        if (bs + need > pend) pend = bs + need;
        if (!soft) { tot += need; continue; }
        uint32_t w = (v->w >> l) ? (v->w >> l) : 1u, h = (v->h >> l) ? (v->h >> l) : 1u;
        tot += ((((uint64_t)w * (uint64_t)bpp) + 15) & ~(uint64_t)15) * h * slices;
        if (need > cmax) cmax = need;
    }
    if (!tot) { v->pkg_bad = 1; return; }
    // 整张要的量 = 各级未压缩字节 + 一份压缩块暂存（逐层复用），再加上每段 16 对齐的最坏零头
    uint64_t need_all = tot + cmax + 32ull * (uint64_t)lv * (uint64_t)la_n + 64ull;
    pkg_stage st;
    if (!stage_begin(&st, need_all)) {
        atomic_fetch_add(&g_pkg_big, 1); pkg_big_said(v, tot, cmax);
        if (++v->pkg_rej >= 8) v->pkg_bad = 1;   // 一直圈不到就别每趟绘制都重算一遍几何 | if it never fits, stop recomputing its geometry every flush
        return;
    }
    to_layout(g_cb, v, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint64_t asc = 0;
    if (st.big) atomic_fetch_add(&g_pkg_bigok, 1);
    if (soft) {
        asc = stage_take(&st, cmax ? cmax : 16);   // 压缩块的暂存按最大那一级算，逐层复用同一块
        if (asc == ~(uint64_t)0) { atomic_fetch_add(&g_pkg_arena, 1); return; }
    }
    int ok = 0;
    for (int l = 0; l < lv; l++) for (int la = 0; la < la_n; la++) {
        uint32_t rb, rows, slices, bp2; uint64_t bs; unsigned th, td;
        if (nvn_deswizzle_geom(&t->d, (unsigned)l, (unsigned)la, &rb, &rows, &slices, &bs, &th, &td, &bp2) || !rb) break;
        uint64_t need = (uint64_t)rb * rows * slices;
        if (!need) continue;
        uint32_t ew = (v->w >> l) ? (v->w >> l) : 1u, eh = (v->h >> l) ? (v->h >> l) : 1u;
        uint64_t ao; uint64_t blen;
        VkBufferImageCopy reg = {
            .imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, (uint32_t)la, 1 },
            .imageOffset = { 0, 0, 0 }, .imageExtent = { ew, eh, slices } };
        if (soft) {
            if (nvn_deswizzle(&t->d, (unsigned)l, (unsigned)la, t->mem, st.map + asc, (size_t)rb)) {
                atomic_fetch_add(&g_pkg_dz, 1); l = lv; break;
            }
            uint64_t drow = (((uint64_t)ew * (uint64_t)bpp) + 15) & ~(uint64_t)15;
            blen = drow * eh * slices;
            ao = stage_take(&st, blen);
            if (ao == ~(uint64_t)0) { atomic_fetch_add(&g_pkg_arena, 1); l = lv; break; }
            if (nvn_bc_decode((uint32_t)t->d.format, st.map + asc, (size_t)rb, ew, eh, st.map + ao, (size_t)drow)) {
                atomic_fetch_add(&g_pkg_fmt, 1); v->pkg_bad = 1; l = lv; break;
            }
            reg.bufferRowLength = (uint32_t)(drow / (uint64_t)bpp); reg.bufferImageHeight = eh;
        } else {
            ao = stage_take(&st, need);
            if (ao == ~(uint64_t)0) { atomic_fetch_add(&g_pkg_arena, 1); l = lv; break; }
            if (nvn_deswizzle(&t->d, (unsigned)l, (unsigned)la, t->mem, st.map + ao, (size_t)rb)) {
                atomic_fetch_add(&g_pkg_dz, 1); l = lv; break;
            }
            blen = need;
        }
        // 存第 0、1 级（第 0 层）：整幅的美术看着都对，可 UI 那些小方块是平涂的 —— 小方块采的是 mip>0，
        // 所以「级别 0 好、级别 1 坏」正好能解释这个分裂；一次跑把两级都落盘，离线各自解一遍就知道。
        // 未压缩那类只有一份线性字节，就只落 A。
        // Levels 0 and 1: full-size art reads correctly while the small UI quads come out flat, and those quads sample mip>0,
        // so "level 0 good, level 1 broken" is exactly the split this dump can decide. Uncompressed images have one buffer, so only A.
        if (la == 0 && l <= 1)
            tex_dump(v->serial, (unsigned)l, (unsigned)lv, soft ? st.map + asc : st.map + ao, (size_t)need,
                     soft ? st.map + ao : NULL, soft ? (size_t)blen : 0,
                     ew, eh, rb, (int)t->d.format, (int)v->fmt, soft);
        if (la == 0 && l == 0) tex_dump_raw(&t->d, v->serial, t->mem, (size_t)avail);   // 整张 + 描述符：离线穷举每级几何
        reg.bufferOffset = ao;
        vkCmdCopyBufferToImage(g_cb, st.buf, v->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
        atomic_fetch_add(&g_pkg_bytes, blen);
        if (soft) atomic_fetch_add(&g_pkg_bc, 1);
        ok = 1;
        if (l == 0 && la == 0) {
            // 探针行得带上格式对与行字节数：第 110 轮同是 1280x720 的两次上传，一次 3 686 400 B（4 B/像素，软解）一次 921 600 B
            //   （1 B/像素），光看尺寸分不清是「按压缩块搬」还是「单通道图」，而这两条的下一步完全不同。
            // The probe line needs the format pair and the row pitch: round 110 measured two 1280x720 uploads at 3 686 400 B
            //   (4 B/pixel, soft-decoded) and 921 600 B (1 B/pixel). Size alone cannot tell a block-format copy from a
            //   single-channel image, and those two need completely different next cuts.
            char tg[56];
            snprintf(tg, sizeof tg, "打包 NVN%d→Vk%d 软%d 行%u", (int)t->d.format, (int)v->fmt, soft, rb);
            up_probe(tg, st.map + ao, blen, ew, eh, 1);
        }
    }
    to_layout(g_cb, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!ok) { v->pkg_bad = 1; return; }
    if (v->uploaded) atomic_fetch_add(&g_pkg_re, 1);   // 重传（游戏又写了这张）| a re-upload: the game wrote this one again
    v->uploaded = 1;
    v->pkg_dirty = 0;
    v->pkg_stale = 0;
    // 上传完把这张图在池里占的页设成只读：游戏再写它就缺页 → 代号前进 → 用之前查到就重传（范围与戳的取法见绑定处那段）
    // After the upload, set the pages this texture occupies read-only: the next guest write faults and bumps the generation, which
    // is what the before-use check turns into a re-upload.
    v->pkg_range = pend; v->pkg_stamp = st0; v->pkg_seen = st0;
    if (pend && !(g_skip & 67108864)) nvn_dirty_protect(t->mem, (size_t)pend);
    atomic_fetch_add(&g_pkg_up, 1);
    static int saidp;
    if (saidp < 8) {
        saidp++;
        GVL("[vk] 打包上传%{public}d s=%{public}llu NVN%{public}d→Vk%{public}d %{public}u x %{public}u 层%{public}u 级%{public}u 软解%{public}d 累计%{public}llu字节",
            saidp, (unsigned long long)v->serial, (int)t->d.format, (int)v->fmt, v->w, v->h,
            v->layers, v->levels, soft, (unsigned long long)g_pkg_bytes);
    }
}

// 通道外冲刷：绑定发生在通道里，那时发不了 transfer，所以先排队，pass_end 统一搬。
// Flush outside the pass: binds happen inside a pass where a transfer cannot be issued, so they queue up and pass_end moves them.
static void pkg_flush(void)
{
    if (g_skip & 4194304) { g_pkg_pend_n = 0; return; }   // 位 4194304 = 打包上传整条关掉，做 A/B
    int n = g_pkg_pend_n;
    g_pkg_pend_n = 0;
    if (!n) return;
    int did = 0;
    for (int i = 0; i < n; i++) {
        vktex *v = g_pkg_pend[i];
        // 排队存的是数组里的指针，中途有图被释放会把末尾那张换位、把这一槽清成 0：img 空了就不能再搬
        // The queue holds pointers into the texture array; a release mid-frame swaps the last entry down and zeroes the vacated slot.
        if (!v || !v->img || v->pkg_bad) continue;
        if (v->uploaded && !v->pkg_dirty && !v->pkg_stale) continue;
        pkg_upload(v);
        if (v->uploaded) did++;
    }
    if (did) g_sets_valid = 0;   // 下一趟绘制重新绑：这些槽该换成真视图 | rebind next pass: these slots now hold real views
}

static void pkg_queue(vktex *v)
{
    if (!v || !v->img || v->pkg_bad) return;
    if (v->uploaded && !v->pkg_dirty && !v->pkg_stale) return;
    if (g_pkg_pend_n >= PKG_PEND) return;
    for (int i = 0; i < g_pkg_pend_n; i++) if (g_pkg_pend[i] == v) return;
    g_pkg_pend[g_pkg_pend_n++] = v;
    // 点名前 40 张进队的图：地面图集到底有没有来过这里（进队=会尝试上传；没进队=绑定路径都没走到）
    // Name the first 40 queued images: did the terrain atlases ever get here? (queued = an upload will be attempted;
    // never queued = the bind path never even reached this point)
    { static _Atomic int nq; int k = atomic_fetch_add(&nq, 1);
      if (k < 40) GVL("[vk] 排队上传 s=%{public}llu %{public}ux%{public}u 格%{public}d 级%{public}u 传过%{public}d",
                      (unsigned long long)v->serial, v->w, v->h, (int)v->fmt, v->levels, v->uploaded); }}

// 「无通道」那一堆到底缺哪一件（第 82 轮：88 万条绘制没通道，比真正重放的还多六倍）
// Which half of the nopass test failed: on round 82 880k draws had no pass - six times more than the 130k that replayed.
static int g_np_said;
static void np_said(int which, const p_draw *dw, const p_draw_idx *di)
{
    if (g_np_said >= 6) return;
    g_np_said++;
    const prog_t *pr = S.prog;
    GVL("[vk] 无通道%{public}d 序%{public}llu 段%{public}u 形%{public}d 起%{public}d 数%{public}d｜在通道%{public}d 有cb%{public}d 顶色%{public}u 通道%{public}p 尺寸%{public}u x %{public}u",
        which, pr ? (unsigned long long)pr->serial : 0ull, pr ? pr->nstage : 0u,
        (int)(dw ? dw->prim : (di ? di->prim : -9)), (int)(dw ? dw->first : -1), (int)(dw ? dw->count : (di ? di->count : -1)),
        g_in_pass, g_cb != NULL, g_p.ncol, (void *)(uintptr_t)g_p.rp,
        g_p.cv[0] ? g_p.cv[0]->w : 0u, g_p.cv[0] ? g_p.cv[0]->h : 0u);
}

// 库里没有的那个程序，把它的段键点名（每轮最多 8 组）。
// 只报「无库 8631」不够用：第 115/116 轮 B40 一条缺键就吃掉 8631 笔绘制，而键要拿去离线出库才能修，
// 计数行不点名就得再靠 DM_VK_KEYDUMP 那份 22 条样本去猜是哪一条。
static char g_nolib_key[8][SH_STAGE_N * 18 + 8];
static int g_nolib_kn;
static void nolib_said(const progrec *p)
{
    if (!p || g_nolib_kn >= 8) return;
    char *s = g_nolib_key[g_nolib_kn];
    size_t cap = sizeof g_nolib_key[0];
    int k = 0;
    for (int i = 0; i < SH_STAGE_N && (size_t)k + 18 < cap; i++)
        if (p->key[i][0]) k += snprintf(s + k, cap - (size_t)k, "%s%s", k ? " " : "", p->key[i]);
    if (!k) snprintf(s, cap, "(无键)");
    for (int i = 0; i + 1 < g_nolib_kn; i++) if (!strcmp(g_nolib_key[i], s)) return;
    g_nolib_kn++;
}

// 上屏那条通道里那一笔绘制的现场：程序、图元、视口、剪裁、写掩码，以及各阶段绑上的贴图有没有内容
static int g_sp_said;
static void sp_draw_said(int vi, int fi, const p_draw *dw, const p_draw_idx *di)
{
    if (g_sp_said >= 6) return;
    g_sp_said++;
    char tb[220]; int o = 0;
    for (int s = 0; s < 6 && o < 190; s++)
        for (int k = 0; k < 12 && o < 190; k++) {
            // 槽里放的是句柄（低 20 位 = 贴图池号），不是序号：按序号查会把每张真图都报成「无图」
            // The slot holds a handle (texture pool index in the low 20 bits), not a serial - looking it up as a serial reported 无图 for every real texture
            uint64_t hd = S.tex[s][k];
            int tid = (int)(hd & 0xfffff);
            if (!tid) continue;
            vktex *x = tex_of_tid(tid);
            o += snprintf(tb + o, sizeof tb - (size_t)o, " %d.%d=t%d%s", s, k, tid,
                          x ? (x->uploaded ? "上" : "空") : "无图");
        }
    tb[o < (int)sizeof tb ? o : (int)sizeof tb - 1] = 0;
    GVL("[vk] 上屏绘%{public}d 序%{public}llu 顶%{public}d 像%{public}d 形%{public}d 起%{public}d 数%{public}d｜顶色%{public}u 尺寸%{public}u x %{public}u 视口%{public}d 遮%{public}d x %{public}d 多层%{public}u 混合%{public}d 写%{public}x｜贴%{public}s",
        g_sp_said, (unsigned long long)(S.prog ? S.prog->serial : 0), vi, fi,
        (int)(dw ? dw->prim : di->prim), (int)(dw ? dw->first : -1), (int)(dw ? dw->count : di->count),
        g_p.ncol, g_p.cv[0] ? g_p.cv[0]->w : 0u, g_p.cv[0] ? g_p.cv[0]->h : 0u, S.has_vp,
        S.has_sc ? S.sc.w : -1, S.has_sc ? S.sc.h : -1, S.ms.samples, S.color.blend_en, (unsigned)S.chmask.mask, tb);
}

// 上屏绘制的混合态普查（B45）| blend-state census for on-screen draws
// 只数目标 0（UI 全画在 0 号上），把 NVN 原样的六个编号 + 混合使能位 + 0 号写掩码当键，按笔数取前 6 名打一行。
// 编号一律十进制，对着 nvn.h 的 NVN_BLEND_FUNC_*（ZERO=1 ONE=2 SRC_COLOR=3 OMSC=4 SRC_ALPHA=5 OMSA=6
// DST_ALPHA=7 OMDA=8 DST_COLOR=9 OMDC=10 SATURATE=11 SRC1_*=16..19 CONSTANT_*=97..100）直接能读。
static struct { uint64_t k; uint32_t n; uint16_t vi, fi; } g_bsp[24];
static int g_bsp_n; static uint64_t g_bsp_tot;

static void bsp_said(void)
{
    char tb[520]; int o = 0; uint32_t used = 0;
    for (int rank = 0; rank < 6; rank++) {
        int best = -1;
        for (int i = 0; i < g_bsp_n; i++)
            if (!((used >> i) & 1u) && (best < 0 || g_bsp[i].n > g_bsp[best].n)) best = i;
        if (best < 0) break;
        used |= 1u << best;
        uint64_t k = g_bsp[best].k;
        // 11 条格式符配 11 个参数：上一版多写了两个 %u，「画」和 eq 读到了栈上的垃圾，六个混合态的因子因此判错了一次
        // 11 specifiers, 11 arguments: the extra pair of %u read stack garbage, so round B46's factor decode was wrong.
        // 因子按换算后的 Vk 编号打印（0零 1一 2源色 4目色 6源a 8目a…），NVN 原值对不上这张表。
        o += snprintf(tb + o, sizeof tb - (size_t)o, " [%d.%d 混%x 写%x 色%u/%u 透%u/%u eq%u/%u 画%u]",
                      g_bsp[best].vi, g_bsp[best].fi, (unsigned)((k >> 34) & 0xFF), (unsigned)((k >> 42) & 0xF),
                      (unsigned)bfac_of((int)(k & 0x7F)), (unsigned)bfac_of((int)((k >> 7) & 0x7F)),
                      (unsigned)bfac_of((int)((k >> 14) & 0x7F)), (unsigned)bfac_of((int)((k >> 21) & 0x7F)),
                      (unsigned)((k >> 28) & 7), (unsigned)((k >> 31) & 7), g_bsp[best].n);
    }
    tb[o < (int)sizeof tb ? o : (int)sizeof tb - 1] = 0;
    // 尾巴这条 %s 必须写 %{public}s：hilog 对没标 public 的参数一律打 <private>（B45 那轮六个混合态的编号就是这么被吞掉的）
    GVL("[vk] 混合普查 上屏%{public}llu 态%{public}d%{public}s", (unsigned long long)g_bsp_tot, g_bsp_n, tb);
}

static void bsp_tally(int vi, int fi)
{
    const blend_st *b = &S.blend[0];
    uint64_t k = (uint64_t)b->src_rgb | ((uint64_t)b->dst_rgb << 7) | ((uint64_t)b->src_a << 14)
               | ((uint64_t)b->dst_a << 21) | ((uint64_t)b->eq_rgb << 28) | ((uint64_t)b->eq_a << 31)
               | ((uint64_t)(S.color.blend_en & 0xFF) << 34) | ((uint64_t)(S.chmask.mask & 0xF) << 42);
    g_bsp_tot++;
    int i;
    for (i = 0; i < g_bsp_n; i++) if (g_bsp[i].k == k) { g_bsp[i].n++; break; }
    if (i == g_bsp_n) {
        int slot = -1;
        if (g_bsp_n < 24) slot = g_bsp_n++;
        else { slot = 0; for (int j = 1; j < g_bsp_n; j++) if (g_bsp[j].n < g_bsp[slot].n) slot = j; }
        g_bsp[slot].k = k; g_bsp[slot].n = 1; g_bsp[slot].vi = (uint16_t)vi; g_bsp[slot].fi = (uint16_t)fi;
    }
    if (!(g_bsp_tot % 20000)) bsp_said();
}

// 顶点色 alpha 普查（B46）| the vertex-colour alpha probe
// B45b 把混合源因子顶成 ONE，整页 UI 美术立刻回来了 ⇒ 这些笔的 RGB 全对、只有源 alpha 是 0。
// 源 alpha 只有三个来路：顶点色、贴图、着色器自己算。贴图那条已经排掉（tex_B42 的 RGBA 直方图有正常分布），
// 所以这里直接读 arena 里搬进来的顶点字节：挑「着色器声明 + NVN 是四分量 8 位」的属性
// （RGBA8=0x25 RGBA8SN=0x26 RGBA8UI=0x27 RGBA8I=0x28），逐顶点看第 4 个字节。
// 全 0 ⇒ 顶点色带的就是 0 alpha（去查客体顶点流/我们的搬运格式）；有值 ⇒ 转去查着色器输出通道。
static int g_va_said;
static void va_alpha_said(int vi)
{
    if (g_va_said >= 10 || !g_arena_map) return;
    const shb_entry *ve = &g_shb_e[vi];
    for (int v = 0; v < ve->nvi; v++) {
        int loc = g_shb_vi[ve->vi_off + v].loc, cls = g_shb_vi[ve->vi_off + v].cls;
        if (loc < 0 || loc >= 16) continue;
        int f = (int)S.attr[loc].fmt;
        if (f < 0x01 || f > 0x30) continue;
        int nc = 1 + (f - 1) / 12, w = (f - 1) % 12;
        if (nc != 4 || w > 3) continue;
        vtx_plan pl; vtx_plan_for(loc, cls, &pl);
        if (pl.constant || pl.binding >= 16) continue;
        uint32_t so = 0;
        if (!vtx_stream((int)pl.binding, &so)) continue;
        uint32_t stride = S.stream[pl.binding].stride;
        if (!stride) continue;
        size_t lim = (size_t)ARENA_SZ - ARENA_GUARD - so - pl.off - 4;
        const uint8_t *base = g_arena_map + so + pl.off;
        uint32_t mn[4] = { 255u, 255u, 255u, 255u }, mx[4] = { 0, 0, 0, 0 }, sum[4] = { 0, 0, 0, 0 }, n = 0;
        for (uint32_t i = 0; i < 512 && (size_t)i * stride <= lim; i++) {
            for (int c = 0; c < 4; c++) {
                uint32_t b = base[(size_t)i * stride + c];
                if (b < mn[c]) mn[c] = b;
                if (b > mx[c]) mx[c] = b;
                sum[c] += b;
            }
            n++;
        }
        if (!n) continue;
        g_va_said++;
        GVL("[vk] 顶点色 序%{public}llu 入%{public}d 形%{public}d 类%{public}d 流%{public}u 距%{public}u 偏%{public}u 补%{public}llu｜%{public}u,%{public}u,%{public}u %{public}u,%{public}u,%{public}u %{public}u,%{public}u,%{public}u %{public}u,%{public}u,%{public}u 顶%{public}u",
            (unsigned long long)(S.prog ? S.prog->serial : 0ull), loc, f, cls, pl.binding, stride, pl.off,
            (unsigned long long)atomic_load(&g_vap_patch),
            mn[0], mx[0], (unsigned)(sum[0] / n), mn[1], mx[1], (unsigned)(sum[1] / n),
            mn[2], mx[2], (unsigned)(sum[2] / n), mn[3], mx[3], (unsigned)(sum[3] / n), n);
    }
}

static void do_draw(const p_draw *dw, const p_draw_idx *di)
{
    if (g_skip & 2048) return;
    if (!(g_in_pass && g_cb)) { g_dr_nopass++; atomic_fetch_add(&g_np_out, 1); np_said(0, dw, di); return; }
    // 只挂深度的通道也要画：第 87 轮的 无通道130838 里，np_said 报的全是「在通道1 顶色0 通道非空」这一类，
    // 也就是游戏开了一趟只有深度附件的通道（深度预通道 / 阴影图），旧判据把整趟绘制全丢了。
    // A depth-only pass must still draw: every 无通道 np_said sampled on round 87 read 「in a pass, zero colour attachments,
    // a real render pass」 - the game's depth pre-pass / shadow map, and the old test dropped all of it.
    if (!g_p.ncol && !g_p.has_ds) { g_dr_nopass++; atomic_fetch_add(&g_np_nocol, 1); np_said(1, dw, di); return; }
    if (!g_p.ncol) atomic_fetch_add(&g_dr_dsonly, 1);
    if (!g_p.rp) { g_dr_nopass++; atomic_fetch_add(&g_np_norps, 1); np_said(2, dw, di); return; }
    if (g_att_src) atomic_fetch_add(&g_sp_draw, 1);
    if (!g_draw_ready && !draw_boot()) return;
    if (!g_cur_prog || g_cur_prog->vi < 0 || g_cur_prog->fi < 0) { g_dr_nolib++; nolib_said(g_cur_prog); sp_why(1, g_cur_prog ? g_cur_prog->vi : -2, g_cur_prog ? g_cur_prog->fi : -2, 0); return; }
    int vi = g_cur_prog->vi, fi = g_cur_prog->fi;
    if (g_sho[vi].bad || g_sho[fi].bad) { g_dr_bad++; sp_why(2, vi, fi, (g_sho[vi].bad << 8) | g_sho[fi].bad); return; }
    if (S.ms.samples > 1) { g_dr_msaa++; sp_why(3, vi, fi, (int)S.ms.samples); return; }
    if (!sho_prep(vi) || !sho_prep(fi)) { g_dr_bad++; sp_why(4, vi, fi, 0); return; }
    if (++g_dr_thissub > g_draw_max) { g_dr_over++; sp_why(5, vi, fi, g_draw_max); return; }
    VkPipeline p = pipe_get(vi, fi, dw ? dw->prim : di->prim, g_p.rp, g_p.pk);
    if (!p) { g_dr_nopipe++; sp_why(6, vi, fi, (int)(dw ? dw->prim : di->prim)); return; }
    if (g_last_pipe != p) {
        vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        g_last_pipe = p;
        VkPipelineLayout pl2 = ply_get(vi, fi);
        g_last_pl = pl2;
        emit_dyn(g_cb, att_min());   // 换管线就重发动态状态（管线不同则动态状态全部作废）| a new pipeline invalidates all dynamic state
    }
    if (!g_sets_valid || g_cur_vi != vi || g_cur_fi != fi || g_args_dirty[0] || g_args_dirty[1]) {
        if (!sets_rebuild(vi, fi)) { g_dr_nopipe++; return; }
    }
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_last_pl, 0, 2, g_cur_set, 0, NULL);
    // 顶点缓冲：按管线的绑定号绑，流没搬进来的绑零缓冲（读出来是 0，不打掉设备）| vertex buffers: unstreamed bindings fall back to the zero buffer
    {
        VkBuffer bufs[17]; VkDeviceSize offs[17];
        for (int i = 0; i < 17; i++) { bufs[i] = g_zero; offs[i] = 0; }
        const shb_entry *ve = &g_shb_e[vi];
        for (int v = 0; v < ve->nvi; v++) {
            int loc = g_shb_vi[ve->vi_off + v].loc;
            vtx_plan pl2; vtx_plan_for(loc, g_shb_vi[ve->vi_off + v].cls, &pl2);
            if (pl2.constant) continue;
            uint32_t o = 0;
            if (vtx_stream((int)pl2.binding, &o)) { bufs[pl2.binding] = g_arena; offs[pl2.binding] = o; }
        }
        vkCmdBindVertexBuffers(g_cb, 0, 17, bufs, offs);
    }
    if (g_att_src) { sp_draw_said(vi, fi, dw, di); bsp_tally(vi, fi);
                    if (S.color.blend_en & 1) va_alpha_said(vi); }   // 只查开了混合的那批：不透明的文字不是嫌疑人
    if (dw) {
        if (dw->count) vkCmdDraw(g_cb, (uint32_t)dw->count, 1, (uint32_t)dw->first, 0);
    } else {
        int esz = di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : 0;
        if (!esz || !di->count) { if (esz == 0) { static int said8; if (!said8++) GVL("[vk] 8 位索引这刀不接"); } return; }
        uint32_t io = 0;
        if (!arena_put(di->ibuf, (uint64_t)di->count * esz, 0, &io)) return;
        vkCmdBindIndexBuffer(g_cb, g_arena, io, esz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(g_cb, (uint32_t)di->count, di->instances > 0 ? (uint32_t)di->instances : 1,
                         0, di->base_vertex, (uint32_t)di->base_instance);
    }
    atomic_fetch_add(&g_dr_replay, 1);
    if (g_att_src) atomic_fetch_add(&g_sp_replay, 1);
    if (g_in1_cur) atomic_fetch_add(&g_in1_draw, 1);
}

static void draw_stats_line(char *line, int m, int cap)
{
    m += snprintf(line + m, (size_t)(cap - m), " 绘:重放%llu 无库%llu 跳过%llu 无管线%llu 超额%llu 无通道%llu 多采样%llu 负视口%llu",
                  (unsigned long long)atomic_load(&g_dr_replay), (unsigned long long)atomic_load(&g_dr_nolib),
                  (unsigned long long)atomic_load(&g_dr_bad), (unsigned long long)atomic_load(&g_dr_nopipe),
                  (unsigned long long)atomic_load(&g_dr_over), (unsigned long long)atomic_load(&g_dr_nopass),
                  (unsigned long long)atomic_load(&g_dr_msaa), (unsigned long long)atomic_load(&g_vp_neg));
    // 无库不为 0 时，光有次数修不了：键才是离线出库的输入
    if (g_nolib_kn) {
        m += snprintf(line + m, (size_t)(cap - m), "｜缺键%d ", g_nolib_kn);
        for (int i = 0; i < g_nolib_kn; i++)
            m += snprintf(line + m, (size_t)(cap - m), "%s%s", i ? " / " : "", g_nolib_key[i]);
    }
    m += snprintf(line + m, (size_t)(cap - m), "｜arena:新%llu 复用%llu 越池%llu 满%llu 集尽%llu 管线%llu/%llu",
                  (unsigned long long)atomic_load(&g_adup_new), (unsigned long long)atomic_load(&g_adup_hit),
                  (unsigned long long)atomic_load(&g_adup_bad), (unsigned long long)atomic_load(&g_arena_full),
                  (unsigned long long)atomic_load(&g_set_oom),
                  (unsigned long long)atomic_load(&g_pipe_new), (unsigned long long)atomic_load(&g_pipe_hit));
    m += snprintf(line + m, (size_t)(cap - m), "｜上传:成%llu 软解%llu 无池%llu 通道内%llu 压缩%llu 无图%llu",
                  (unsigned long long)atomic_load(&g_b2t_ok), (unsigned long long)atomic_load(&g_b2t_soft),
                  (unsigned long long)atomic_load(&g_b2t_noaddr), (unsigned long long)atomic_load(&g_b2t_inpass),
                  (unsigned long long)atomic_load(&g_b2t_comp), (unsigned long long)atomic_load(&g_b2t_dst));
    // 无通道那一半的原因：附件没解出 / 没有命令缓冲 / 渲染通道或帧缓冲建不出来
    // The 无通道 half: attachments did not resolve / no command buffer / render pass or framebuffer creation failed
    m += snprintf(line + m, (size_t)(cap - m), "｜通道:无附件%llu 无cb%llu 无rp%llu 无fb%llu 已开%llu",
                  (unsigned long long)atomic_load(&g_rt_noatt), (unsigned long long)atomic_load(&g_rt_nocb),
                  (unsigned long long)atomic_load(&g_rt_rp), (unsigned long long)atomic_load(&g_rt_fb),
                  (unsigned long long)atomic_load(&g_passes));
    // 着色器侧：建成的模块数 / vkCreateShaderModule 失败数 / 按黑名单删掉的能力声明条数 / 三处机械改写的段数
    // Shader side: modules built, failed module creations, capabilities deleted by the blacklist, and how many modules got each mechanical rewrite
    m += snprintf(line + m, (size_t)(cap - m), "｜着色:模块%llu 建败%llu 删能力%llu 管线败%llu（组内%llu 组外%llu）",
                  (unsigned long long)atomic_load(&g_sho_ok), (unsigned long long)atomic_load(&g_mod_fail),
                  (unsigned long long)atomic_load(&g_cap_drop), (unsigned long long)atomic_load(&g_pipe_fail),
                  (unsigned long long)atomic_load(&g_pipe_fail_scop), (unsigned long long)atomic_load(&g_pipe_fail_plain));
    m += snprintf(line + m, (size_t)(cap - m), "｜改写:左下%llu SSBO%llu 删扩展%llu 缓冲维改1D%llu",
                  (unsigned long long)atomic_load(&g_oll_fix), (unsigned long long)atomic_load(&g_bb_fix),
                  (unsigned long long)atomic_load(&g_ext_drop), (unsigned long long)atomic_load(&g_bufdim_fix));
    // 绑图：第 84 轮之前 mtl_reg_tex 是空函数，所有采样都绑在 1x1 哑图上——这是黑屏的头号根因
    // Texture bindings: before round 85 mtl_reg_tex was an empty stub, so every sample read a 1x1 dummy. That is the primary root cause of the black screen.
    m += snprintf(line + m, (size_t)(cap - m), "｜绑图:实%llu 哑%llu 无号%llu 缓冲%llu 布局未就%llu 登记%llu 同槽换图%llu",
                  (unsigned long long)atomic_load(&g_bind_real), (unsigned long long)atomic_load(&g_bind_dummy),
                  (unsigned long long)atomic_load(&g_bind_none), (unsigned long long)atomic_load(&g_bind_buf),
                  (unsigned long long)atomic_load(&g_bind_lay), (unsigned long long)atomic_load(&g_tex_reg),
                  (unsigned long long)atomic_load(&g_tex_rereg));
    // 打包上传（M2-3f）：搬了几张、多少字节、各自卡在哪一环 | the packaged upload path: how many moved, how many bytes, and where each refusal came from
    m += snprintf(line + m, (size_t)(cap - m), "｜打包:排%llu 上%llu 重%llu 成%llu MB BC层%llu 卡arena%llu 越界%llu 解排%llu 格式%llu 深度%llu 三维%llu 太大%llu 巨图成%llu",
                  (unsigned long long)atomic_load(&g_pkg_q), (unsigned long long)atomic_load(&g_pkg_up),
                  (unsigned long long)atomic_load(&g_pkg_re),
                  (unsigned long long)(atomic_load(&g_pkg_bytes) >> 20),
                  (unsigned long long)(atomic_load(&g_pkg_bytes) >> 20),
                  (unsigned long long)atomic_load(&g_pkg_bc),
                  (unsigned long long)atomic_load(&g_pkg_arena), (unsigned long long)atomic_load(&g_pkg_range),
                  (unsigned long long)atomic_load(&g_pkg_dz), (unsigned long long)atomic_load(&g_pkg_fmt),
                  (unsigned long long)atomic_load(&g_pkg_dep), (unsigned long long)atomic_load(&g_pkg_3d),
                  (unsigned long long)atomic_load(&g_pkg_big), (unsigned long long)atomic_load(&g_pkg_bigok));
    // 按内容比对判脏：这条是「视频冻在头几帧、屏幕每四拍闪一下」那一类的主判据（缺页那条在本机不响）
    // The content-compare watch: this is the judge for "the video is stuck on its first frames and the screen strobes every four
    // presents", since the write-protect fault path stays silent on this device.
    m += snprintf(line + m, (size_t)(cap - m), "｜池判脏:变%llu 排%llu 顶%llu 没变%llu",
                  (unsigned long long)atomic_load(&g_pw_chg), (unsigned long long)atomic_load(&g_pw_que),
                  (unsigned long long)atomic_load(&g_pw_cap), (unsigned long long)atomic_load(&g_pw_same));
}

// 每次只打新增的那几条，避免每 300 个 present 把同一串清单重复刷进日志
// Only the newly recorded scenes go out, so the same list is not re-spammed every 300 presents
static void pipe_fail_dump(void)
{
    static int shown;
    int n = atomic_load(&g_pf_n);
    for (; shown < n; shown++) {
        const pf_ent *p = &g_pf[shown];
        GVL("[vk] PF %d %d f=%02x/%02x p=%d r=%d pv=%d pf=%d",
            (int)p->vi, (int)p->fi, p->fv, p->ff, (int)p->prim, (int)p->r, (int)p->pv, (int)p->pf);
    }
}

void mtl_exec(const uint8_t *handle)
{
    if (!handle || !vk_ensure()) return;
    const uint8_t *p = handle;
    int top = g_ret_n;
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (!h.size) { GRED("命令记录 size=0（op=%{public}d）：停在这条", (int)h.op); g_ret_n = top; return; }
        const uint8_t *body = p + sizeof h;
        if (h.op >= OP_COUNT_) { GRED("没见过的 op=%{public}d", (int)h.op); g_ret_n = top; return; }
        atomic_fetch_add(&g_ops[h.op], 1);
        if (h.op < OP_COUNT_) g_buf_ops[h.op]++;
        switch (h.op) {
        case OP_END:
            if (g_ret_n > top) { p = g_ret[--g_ret_n]; continue; }
            return;
        case OP_JUMP: { uint64_t t; memcpy(&t, body, 8); p = (const uint8_t *)(uintptr_t)t; continue; }   // 尾巴跳转，不返回 | tail jump, no return
        case OP_CALL: { uint64_t t; memcpy(&t, body, 8);
            if (g_ret_n >= EXEC_DEPTH) { GRED("命令嵌套超 %{public}d 层：这条 call 跳过", EXEC_DEPTH); break; }
            g_ret[g_ret_n++] = p + h.size;
            p = (const uint8_t *)(uintptr_t)t;
            continue; }
        case OP_SET_RT: if (!(g_skip & 1)) do_set_rt((const p_set_rt *)body); break;   // 位 1 = 游戏侧一个通道都不开：出图的锅和游戏的锅就此分开
        case OP_CLEAR_COLOR: if (!(g_skip & 2)) do_clear_color((const p_clear_color *)body); break;
        case OP_CLEAR_DS: if (!(g_skip & 2)) do_clear_ds((const p_clear_ds *)body); break;
        case OP_BEGIN: g_cur_prog = NULL; g_bind_vi = g_bind_fi = -1; break;   // 一条新录制从「没有程序」开始，与 NVN 的命令缓冲状态一致 | a fresh recording starts with no program bound, like NVN's command buffer state
        case OP_BIND_PROGRAM: do_bind_program((const p_bind_program *)body); state_op(h.op, body); break;
        case OP_DRAW: {
            const p_draw *d = (const p_draw *)body;
            atomic_fetch_add(&g_draws, 1);
            do_draw_count(d->prim, -1);
            do_draw(d, NULL);
            break; }
        case OP_DRAW_IDX: {
            const p_draw_idx *d = (const p_draw_idx *)body;
            atomic_fetch_add(&g_draws, 1);
            do_draw_count(d->prim, d->itype);
            do_draw(NULL, d);
            break; }
        case OP_COPY_B2T: atomic_fetch_add(&g_copies, 1); do_copy_b2t((const p_copy_b2t *)body); break;
        case OP_COPY_B2B: case OP_UPDATE_UB: case OP_COPY_T2B: case OP_COPY_T2T:
        case OP_CLEAR_TEX: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM:
        case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP:
        case OP_BIND_IMG: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_COPY_STRIDE: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
        case OP_DISPATCH: case OP_BARRIER:
            atomic_fetch_add(&g_dispatches, h.op == OP_DISPATCH ? 1 : 0);
            atomic_fetch_add(&g_copies, (h.op == OP_COPY_B2B || h.op == OP_COPY_B2T || h.op == OP_COPY_T2B || h.op == OP_COPY_T2T) ? 1 : 0);
            state_op(h.op, body);   // 状态类进镜像 S，纯记账类（拷贝 / 派发 / 池）在里面直接返回 0 | state ops update the mirror S; the pure-bookkeeping ones return 0
            break;
        case OP_SIGNAL_EVENT: {
            // 记录 24 字节：+8 指针、+16 mode(u32)、+20 value(u32)（hle_nvn.c:523-525）；value 在 body 的第 12 字节，别越过记录尾
            // 24-byte record: +8 pointer, +16 mode (u32), +20 value (u32) (hle_nvn.c:523-525); value sits at body byte 12, not past the record
            const uint8_t *b = body;
            uint64_t val; uint32_t m, v;
            memcpy(&val, b, 8); memcpy(&m, b + 8, 4); memcpy(&v, b + 12, 4);
            mtl_sub_event((volatile uint32_t *)(uintptr_t)val, m, v);
            break; }
        case OP_REPORT_COUNTER: {
            // 载荷顺序是 {类型, 地址}（nvn_mtl.m:5067 为准），弄反就是把类型号 1 当指针写
            // Payload order is {type, address} (per nvn_mtl.m:5067); reversed means writing through the type number as a pointer
            const uint64_t *a = (const uint64_t *)body;
            mtl_sub_counter(a[1], (uint32_t)a[0]);
            break; }
        case OP_RESET_COUNTER: break;
        default: break;
        }
        p += h.size;
    }
    g_ret_n = top;
    GRED("一条句柄里跑了 20 万次记录，停");
}

// CPU 侧副作用：事件与计数器（游戏会轮询它们，必须立刻生效）
// CPU-visible side effects: events and counters (the game polls them, so they must take effect immediately)
static int host_writable(const void *p)
{
    if (!p) return 0;
    unsigned char vec = 0;
    return mincore((void *)((uintptr_t)p & ~(uintptr_t)0xFFF), 4096, &vec) == 0;
}

void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v)
{
    if (!host_writable(val)) return;
    if (mode == 1) __atomic_fetch_add((uint32_t *)val, v, __ATOMIC_SEQ_CST);
    else __atomic_store_n((uint32_t *)val, v, __ATOMIC_SEQ_CST);
}

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n)
{
    // 本刀不动数据：与 M1 的空设备一致，只记账。CPU 直接搬看着省事，但 src/dst 是游戏给的
    // GPU 地址，本层的「GPU 地址 = CPU 地址」只在池内成立——真搬要等描述符/池镜像那一刀，
    // 现在搬等于赌地址有效。
    // This cut leaves the data alone: bookkeeping only, exactly like M1's null device. A CPU memcpy looks cheap, but src/dst are GPU
    // addresses from the game and "GPU address = CPU address" here only holds inside a pool; moving bytes waits for the descriptor/pool
    // mirroring cut - doing it now would be a bet on address validity.
    (void)src; (void)dst; (void)n;
    atomic_fetch_add(&g_copies, 1);
}

void mtl_sub_counter(uint64_t addr, uint32_t type)
{
    (void)type;
    if (!host_writable((const void *)(uintptr_t)addr)) return;
    __atomic_store_n((uint32_t *)(uintptr_t)addr, 0, __ATOMIC_SEQ_CST);   // 没有 GPU 计数：写 0，游戏不卡 | no GPU counter: write 0 so the game does not stall
}

// ---------------- 池与地址反查 | pools ----------------
// Maleoon 没有 VK_EXT_external_memory_host（第 34 轮实测 98 个扩展里就没这条）：本刀仍把 CPU 地址当句柄用，
// 真上传走 mtl_sub_copy / 后续刀的脏区间。池表只记账，让 mtl_addr 能回答「这块在不在池里」。
// Maleoon has no VK_EXT_external_memory_host (absent from the 98 measured extensions): this cut keeps using the CPU address as a handle,
// with real uploads going through mtl_sub_copy / dirty ranges in later cuts. The pool table only keeps bookkeeping so mtl_addr can answer "is this in a pool".
#define POOL_MAX 24
typedef struct { uint64_t lo, hi, base; uint8_t *mem; } pool_ent;
static pool_ent g_pools[POOL_MAX];
static int g_npools;
static _Atomic uint64_t g_pool_adds;

void mtl_pool_add(const void *mem, uint64_t size)
{
    if (!mem || !size) return;
    uint64_t pg = 4096;
    uint64_t lo = (uint64_t)mem & ~(pg - 1), hi = ((uint64_t)mem + size + pg - 1) & ~(pg - 1);
    for (int i = 0; i < g_npools; i++) if (g_pools[i].lo == lo) return;
    if (g_npools >= POOL_MAX) { GRED("池表满了"); return; }
    g_pools[g_npools++] = (pool_ent){ lo, hi, lo, (uint8_t *)mem };
    atomic_fetch_add(&g_pool_adds, 1);
}

void mtl_pool_del(const void *mem)
{
    if (!mem) return;
    uint64_t lo = (uint64_t)mem & ~(4096ull - 1);
    for (int i = 0; i < g_npools; i++) if (g_pools[i].lo == lo) { g_pools[i] = g_pools[--g_npools]; return; }
}

int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off)
{
    if (buf) *buf = NULL;
    if (off) *off = 0;
    if (!g_on) return 0;
    for (int i = 0; i < g_npools; i++)
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
            if (buf) *buf = (void *)(uintptr_t)g_pools[i].lo;
            if (off) *off = addr - g_pools[i].base;
            return 1;
        }
    return 0;
}

int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu)
{
    if (gpu) *gpu = addr;   // 本刀 GPU 地址 = CPU 地址（真描述符绑定时换成 buffer+offset）| this cut: GPU address = CPU address (a later cut replaces this with buffer+offset)
    return mtl_addr(addr, len, buf, off);
}

// ---------------- 其余入口：本刀只记账 | remaining entries: bookkeeping only in this cut ----------------
// 惰性起层：XComponent 表面比宿主线程晚到，所以「没窗口」不算失败，下次入口再试；
// 「有窗口但建不起来」才钉死不重试（并只报一次）。
// Lazy boot: the XComponent surface arrives after the host thread starts, so "no window yet" is not a failure and the next entry retries;
// only "window present but setup failed" is pinned off (and reported once).
static int g_tried;
static int vk_ensure(void)
{
    if (g_on) return 1;
    if (g_tried) return 0;
    const char *e = getenv("DM_VK_GFX");
    if (e && e[0] == '0') { GVL("[vk] DM_VK_GFX 0：不建 Vulkan 层（黑屏基线）"); g_tried = 1; return 0; }
    void *win; uint64_t w = 0, h = 0;
    if (!dm_ohos_window(&win, &w, &h)) return 0;
    g_tried = 1;
    { const char *s = getenv("DM_VK_SKIP"); g_skip = s ? (int)strtol(s, NULL, 0) : 0;
      GVL("[vk] DM_VK_SKIP = %{public}d（1 无通道 2 无清除 4 无深度 8 不出图 16 不搬像素 32 不呈现 64 呈现不用信号量 128 出图前排空队列 256 游戏提交不上 GPU 512 出图写离屏 1024 不画三角形 2048 不重放游戏绘制 4096 颜色回附件态 8192 深度回附件态且不绑真深度 16384 颜色一律用哑图 32768 关掉按需建图 65536 关掉「按池偏移认图+释放进回收站」 131072 按需建图上限抬到 12000 262144 不剔背面 524288 不测深度 1048576 绕序回退成 Metal 那份 8388608 不翻视口 4194304 不搬打包贴图 16777216 采样一律 0 级（禁 mip） 33554432 哑图退回彩色渐变（不设=纯黑） 67108864 打包贴图只传一次 134217728 顶点 RGBA8 的 A 顶成 0xFF 268435456 混合源因子一律顶成 ONE 536870912 关掉「新生命补一次打包重传」1073741824 出图钉死采第一张源图）", g_skip); }   // 逐类关掉的定位开关 | per-class isolation knob
    { const char *s = getenv("DM_FLK"); g_flk_left = s ? (int)strtol(s, NULL, 0) : 0;   // 闪屏探针：逐拍出图整帧读回，打 16x9 亮度网格与周期 | per-present full-frame readback
      const char *t = getenv("DM_RAW"); g_raw_left = t ? (int)strtol(t, NULL, 0) : 0;   // 帧落盘：每 500 拍一轮连拍 4 帧，整帧写裸 RGBA8 文件 | ship n frames to files
      if (g_flk_left || g_raw_left) { g_raw_gap = 300; GVL("[vk] DM_FLK = %{public}d DM_RAW = %{public}d（出图整帧读回；每次内容变化连拍 2 帧、至少隔 150 拍，整帧写文件）", g_flk_left, g_raw_left); } }
    { const char *s = getenv("DM_VK_RB"); g_rb_on = s ? (int)strtol(s, NULL, 0) : 1;   // 出图源读回：默认开（每 300 帧一次，一次一张图的量）
      GVL("[vk] DM_VK_RB = %{public}d", g_rb_on); }
    { const char *s = getenv("DM_POOLWATCH"); g_poolwatch = s ? (int)strtol(s, NULL, 0) : 1;   // 打包贴图按池内容比对判脏（默认开；=0 退回只等缺页）
      GVL("[vk] DM_POOLWATCH = %{public}d", g_poolwatch); }
    g_on = vk_boot();
    if (g_on) { vk_read_limits(); probe_bc(); }   // 常量段区间上限要在建描述符前拿到 | the uniform-range caps must be measured before any descriptor is written
    GVL("[vk] NVN→Vulkan 重放层 g_on=%{public}d 链 %{public}s 表面 %{public}u x %{public}u", g_on, DMSH_CHAIN, (unsigned)w, (unsigned)h);
    return g_on;
}

void mtl_init(void) { vk_ensure(); }

void bf_want_note(const char *key) { (void)key; }
void mtl_set_tex_pool_n(int n) { (void)n; }
// 贴图不在这里建：本层按需创建（do_set_rt / mtl_present 才真挂到通道或出图），
// 否则一个菜单会把上千张登记过的贴图全变成 VkImage。
// No texture is created here: the layer creates on demand (only when do_set_rt / mtl_present actually attach or present),
// otherwise a single menu would turn every registered texture into a VkImage.
// 贴图登记不在这里：真现在上面「贴图表」一节（第 85 轮补，之前是空函数——着色器因此永远采不到游戏的图）。
// Texture registration lives with the texture table above (added in round 85 - it used to be an empty stub, which is why no shader ever sampled a game texture).
// 采样器登记同理，第 88 轮搬到「哑资源」一节之后（那边建 VkSampler 要 g_dev）。
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) { (void)on; (void)px; (void)py; (void)grid; (void)cx; (void)cy; }
int mtl_ui_ghost_rect(float r[4]) { (void)r; return 0; }

void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img)
{
    (void)level; (void)layer0; (void)r;
    if (!dst) return;
    memset(dst, 0, row * img);
    if (!g_on || !t || !t->mem || !t->pool) return;
    // 本刀：直接从池的 CPU 内存按块线性搬（解排布在后续刀接 nvn_deswizzle）。
    // 但长度是客体给的，读出池尾就是又一次 SIGSEGV —— 先按池大小夹一刀。
    // This cut: copy the block-linear bytes straight out of the pool (deswizzling comes with nvn_deswizzle in a later cut).
    // The length comes from the guest though, and reading past the end of the pool is just another SIGSEGV - so clamp to the pool first.
    size_t n = row * img;
    uint64_t off = nvn_tex_level_offset(&t->d, level);
    uint64_t base = t->off > 0 ? (uint64_t)t->off : 0;   // t->mem 已经指到池内这张贴图的开头 | t->mem already points at this texture inside the pool
    uint64_t avail = t->pool->size > base ? t->pool->size - base : 0;
    if (off >= avail || n > avail - off) {
        static int once;
        if (++once <= 4) GRED("读回贴图越界：池 %{public}llu + 偏移 %{public}llu 装不下 %{public}llu 字节",
                             (unsigned long long)t->pool->size, (unsigned long long)off, (unsigned long long)n);
        return;
    }
    memcpy(dst, t->mem + off, n);
}

void mtl_stats(uint64_t *passes, uint64_t *draws)
{
    if (passes) *passes = atomic_load(&g_passes);
    if (draws) *draws = atomic_load(&g_draws);
}
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { if (enc) *enc = 0; if (reuse) *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { if (call) *call = 0; if (skip) *skip = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { if (load) *load = 0; if (store) *store = 0; if (full_rw_passes) *full_rw_passes = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { if (lib) *lib = 0; if (pso) *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { if (uploads) *uploads = 0; if (reuploads) *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
void mtl_alloc_stats(uint64_t *aw, uint64_t *ag, int *an) { if (aw) *aw = 0; if (ag) *ag = 0; if (an) *an = 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb, int heap_ok, unsigned long long heap_mb, unsigned long long heap_free_mb, unsigned long long heap_maxblk_kb)
{ (void)frame; (void)rss_mb; (void)heap_ok; (void)heap_mb; (void)heap_free_mb; (void)heap_maxblk_kb; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0.0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }
int mtl_has_window(void) { void *w; uint64_t a, b; return g_on && dm_ohos_window(&w, &a, &b); }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns) { (void)timeout_ns; return 0; }
double mtl_presented_time(uint64_t n) { (void)n; return 0.0; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_fps_target(void) { return 60; }
// 报 0 = 让出图节拍停在 M1 的 game 模式（hl_evn present_cfg 里 scr>0 会切到 display 剪口）。
// 真机刷新率等画面能稳定出图那一刀再报，避免同时改两个变量。
// Reporting 0 keeps the present cadence in M1's game mode (present_cfg switches to the display cut when scr > 0).
// The real refresh rate goes in once the picture is stable - not two variables in one cut.
int mtl_display_hz(void) { return 0; }
int dm_nvn_ruler_truth(void) { return 0; }
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = 0; if (viol) *viol = 0; }
void dm_gfx_shot_at(long off, const char *path) { (void)off; (void)path; }

void mtl_res_counts(uint32_t tick, const char *why)
{
    if (!g_on) return;
    static int n;
    if (++n > GLOG_MAX || (tick % 300)) return;
    GVL("[vk] 资源(%{public}s 拍 %{public}u)：池 %{public}d、贴图 %{public}d（新 %{public}llu 命中 %{public}llu）、提交 %{public}llu 出图 %{public}llu 通道 %{public}llu 清 %{public}llu 复制 %{public}llu",
        why ? why : "-", tick, g_npools, g_tex_n, (unsigned long long)g_tex_new, (unsigned long long)g_tex_hit,
        (unsigned long long)g_submits, (unsigned long long)g_presents,
        (unsigned long long)atomic_load(&g_passes), (unsigned long long)atomic_load(&g_clears), (unsigned long long)atomic_load(&g_copies));
}

void mtl_res_snapshot(mtl_res_count *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->pools = (uint32_t)g_npools;
    out->pool_adds = atomic_load(&g_pool_adds);
    out->live_tex = (uint32_t)g_tex_n;
    out->dss = (uint32_t)atomic_load(&g_passes);
}
