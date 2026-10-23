// OHOS 入口（NAPI 模块 libdmhost.so）v2：
//  - start(dir, wantParams, resourceDir)：ArkTS 传沙箱 files 目录 + Want 参数 JSON + HAP 资源目录 →
//    建 save/temp 子目录、把 DM_ROMFS 挂到包内只读资源目录（游戏数据在包里，不落地）、设 DM_SAVE/DM_TEMP、chdir，
//    再把宿主 main 挂到独立线程跑（argv[1] = 该目录下的 *.patched.bin 所在目录）。
//  - stderr/stdout 重定向到管道 → hilog 桥线程：设备上取宿主全部日志的唯一通道。
#include "napi/native_api.h"
#include "xcomp_ohos.h"
#include "pad_ohos.h"
#include "front_ohos.h"
#include "vk_smoke.h"
#include <hilog/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "D2R"
#define LOG_DOMAIN 0x3300
#define DLOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

extern int main(int argc, char **argv);

static volatile int g_started;
static char g_dir[1024];
static char g_res[1024];   // HAP 资源目录（ArkTS 的 ctx.resourceDir）→ $: 的候选挂载点

// ---- stderr/stdout → hilog 桥 ----
static int g_pipe[2] = {-1, -1};
static int g_out_redirected;
static void *bridge_main(void *p)
{
    (void)p;
    FILE *f = fdopen(g_pipe[0], "r");
    if (!f) return NULL;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "%{public}s", line);
    }
    return NULL;
}

static void redirect_output(void)
{
    if (g_out_redirected) return;
    g_out_redirected = 1;
    if (pipe(g_pipe)) return;
    fflush(stdout);
    fflush(stderr);
    dup2(g_pipe[1], 1);
    dup2(g_pipe[1], 2);
    close(g_pipe[1]);
    pthread_t t;
    if (!pthread_create(&t, NULL, bridge_main, NULL)) pthread_detach(t);
}

__attribute__((section(".d2rtest"), aligned(4096))) volatile char g_testpage[4096] = {0x11};

// ---- OHOS W^X 规则探针：一次刻清「哪种可执行映射被允许」 ----
static void mmap_probe(const char *dir)
{
    void *p = NULL;
    for (int rep = 0; rep < 3; rep++) {
        p = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        DLOG("[probe] t1#%{public}d anon mmap(RWX 一次给): %{public}s errno=%{public}d", rep, p == MAP_FAILED ? strerror(errno) : "OK", p == MAP_FAILED ? errno : 0);
        if (p != MAP_FAILED) {
            memset(p, 0, 4096);
            // mov w0, #0x42 ; ret —— 可验证载荷：返回 0x42 证明执行的确实是本页指令
            uint32_t code[2] = {0x52800840u, 0xd65f03c0u};
            memcpy(p, code, sizeof code);
            __builtin___clear_cache((char *)p, (char *)p + 4096);   // ★ 关键：刷指令缓存（上一版漏了 → 假崩溃）
            int (*f1)(void) = (int (*)(void))p;
            int v1 = f1();
            DLOG("[probe] t1#%{public}d 执行 RWX 页: %{public}s (ret=%{public}#x)", rep, v1 == 0x42 ? "OK" : "返回值不对", v1);
            munmap(p, 65536);
        }
        p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            memset(p, 0, 4096);
            uint32_t code2[2] = {0x52800840u, 0xd65f03c0u};
            memcpy(p, code2, sizeof code2);
            int r = mprotect(p, 65536, PROT_READ | PROT_EXEC);
            DLOG("[probe] t2#%{public}d anon RW -> mprotect(RX)（W→X 转换）: %{public}s errno=%{public}d", rep, r ? strerror(errno) : "OK", r ? errno : 0);
            if (!r) {
                __builtin___clear_cache((char *)p, (char *)p + 4096);
                int (*f2)(void) = (int (*)(void))p;
                int v2 = f2();
                DLOG("[probe] t2#%{public}d 执行: %{public}s (ret=%{public}#x)", rep, v2 == 0x42 ? "OK" : "返回值不对", v2);
            }
            munmap(p, 65536);
        } else {
            DLOG("[probe] t2#%{public}d anon RW 初映射就失败: %{public}s errno=%{public}d", rep, strerror(errno), errno);
        }
        usleep(60000);
    }

    char path[1200];
    snprintf(path, sizeof path, "%s/main.patched.bin", dir);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        p = mmap(NULL, 65536, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
        DLOG("[probe] t3 file mmap(R|X): %{public}s errno=%{public}d", p == MAP_FAILED ? strerror(errno) : "OK", p == MAP_FAILED ? errno : 0);
        if (p != MAP_FAILED) munmap(p, 65536);

        p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            int r = mprotect(p, 65536, PROT_READ | PROT_EXEC);
            DLOG("[probe] t4 file RW -> mprotect(RX): %{public}s errno=%{public}d", r ? strerror(errno) : "OK", r ? errno : 0);
            munmap(p, 65536);
        } else {
            DLOG("[probe] t4 file RW 初映射失败: %{public}s errno=%{public}d", strerror(errno), errno);
        }
        close(fd);
    } else {
        DLOG("[probe] t3 打不开 %{public}s: %{public}s", path, strerror(errno));
    }
    DLOG("[probe] t5 基线段(.so text)执行中 = OK（本函数就在 .so 里跑）");

    // t6/t7：把 .so 代码段的一页「降权」成 RW（去 X 加 W），再试转回 RX——决定 embed 方案形态
    void *pg = (void *)((uintptr_t)&g_testpage & ~(uintptr_t)0xFFF);
    int r6 = mprotect(pg, 4096, PROT_READ | PROT_WRITE);
    DLOG("[probe] t6 .so代码页 mprotect(RW 去X): %{public}s errno=%{public}d", r6 ? strerror(errno) : "OK", r6 ? errno : 0);
    int r7 = mprotect(pg, 4096, PROT_READ | PROT_EXEC);
    DLOG("[probe] t7 再 RW 转回 RX: %{public}s errno=%{public}d", r7 ? strerror(errno) : "OK", r7 ? errno : 0);
}

static void list_rec(const char *root, int depth, int *budget);   // 诊断用，定义在 host_thread 后 | For diagnostics, defined after host_thread
static void dump_file_log(const char *path, int max_lines);

// ---- RomFS 挂载点选择：数据打进安装包（rawfile），$:` 直接指到包内只读资源目录 ----
// 不再做「壳 + 外部数据」两件事：游戏资源全量进 HAP，测试时只推不含游戏数据的那一份。
// 候选按顺序试，第一个「配置读得到」的胜出；全都不行才退回沙箱 files/romfs。
// 包内那份配置叫 build.config：hvigor 打 rawfile 时**丢点文件**（实测 .build.config 不进包，子目录正常收），
// 客体要的 $:/data/.build.config 由 hle_fs.c 的 path_component 去点回退认到它。
// 标记对：第一列是「这确实是游戏资源根」的判定文件，第二列用来就地读验（能 stat 不代表能 open），第三列是命中时打的标签
//（也是本轮产物的自检字串：装机前 grep 包内 .so 能不能 grep 到它）。
// 第一组是饥荒（CASC 分片），第二组是 Unity（高海 Saga DX：根下直接是 Data/，没有分片档案，拿最大的 assets 顶）。
static const char *kRomfsMarks[][3] = {
    { "data/.build.config",      "data/00-00000001.data", "DST" },
    { "data/build.config",       "data/00-00000001.data", "DST" },
    { "Data/globalgamemanagers", "Data/resources.assets", "Unity-HSS-v33" },
};
static int romfs_has_cfg(const char *dir)
{
    for (size_t i = 0; i < sizeof kRomfsMarks / sizeof kRomfsMarks[0]; i++) {
        char p[1500];
        snprintf(p, sizeof p, "%s/%s", dir, kRomfsMarks[i][0]);
        struct stat st;
        if (!stat(p, &st) && S_ISREG(st.st_mode)) {
            DLOG("[romfs] 配置命中 %{public}s（%{public}lld B）标记=%{public}s", p, (long long)st.st_size, kRomfsMarks[i][2]);
            // 就地读验：包内目录是只读挂载 + SELinux，拿大档案的头 16 字节试
            char ap[1600];
            snprintf(ap, sizeof ap, "%s/%s", dir, kRomfsMarks[i][1]);
            int fd = open(ap, O_RDONLY);
            if (fd < 0) DLOG("[romfs] 档案 open %{public}s 失败 errno=%{public}d", ap, errno);
            else { char b[16]; ssize_t r = read(fd, b, sizeof b); close(fd);
                   DLOG("[romfs] 档案就地读 %{public}s → %{public}zd B，头四字节 %{public}x", ap, r, r >= 4 ? *(unsigned *)b : 0u); }
            return 1;
        }
    }
    return 0;
}

static int pick_romfs(const char *res, const char *dir, char *out, size_t n)
{
    const char *cand[5];
    int nc = 0;
    static char c0[1400], c1[1400], c2[1400], c3[1400], c4[1400];
    if (res && res[0]) {
        snprintf(c0, sizeof c0, "%s/resfile/romfs", res);
        snprintf(c1, sizeof c1, "%s/rawfile/romfs", res);
        snprintf(c2, sizeof c2, "%s/romfs", res);
        snprintf(c3, sizeof c3, "%s", res);
        cand[nc++] = c0; cand[nc++] = c1; cand[nc++] = c2; cand[nc++] = c3;
    }
    snprintf(c4, sizeof c4, "%s/romfs", dir);
    cand[nc++] = c4;
    for (int i = 0; i < nc; i++) {
        DLOG("[romfs] 候选 %{public}s", cand[i]);
        if (romfs_has_cfg(cand[i])) { snprintf(out, n, "%s", cand[i]); return 1; }
    }
    snprintf(out, n, "%s", cand[nc - 1]);
    DLOG("【红】候选都没有配置：$: = %{public}s", out);
    return 0;
}

// 资源分包：签名器硬限单个 HAP 的 zip <4 GB（实测 5.25 GB 报 11017001 find zip central directory failed），
// 所以 5.18 GB 档案拆到多个模块的 resfile 里。$: 挂带配置的那个模块，其余模块的 resfile/romfs 串成
// DM_ROMFS_OVERLAY（冒号分隔，hle_fs.c 里优先于挂载点被查），游戏看到的仍是一个完整的 $:/data。
static void romfs_overlay_from_bundle(char *out, size_t n, const char *mount)
{
    char root[1400];
    const char *p = strstr(g_res, "/resources/resfile");
    if (!p) return;
    size_t bl = (size_t)(p - g_res);                 // 去掉 /<模块>/resources/resfile，剩下 bundle 根
    while (bl && g_res[bl - 1] != '/') bl--;
    if (!bl) return;
    snprintf(root, sizeof root, "%.*s", (int)(bl - 1), g_res);
    DIR *d = opendir(root);
    if (!d) { DLOG("[romfs] 分包根 %{public}s 打不开", root); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char cand[1500];
        snprintf(cand, sizeof cand, "%s/%s/resources/resfile/romfs", root, e->d_name);
        if (!strcmp(cand, mount)) continue;
        char probe[1600];
        snprintf(probe, sizeof probe, "%s/data", cand);
        struct stat st;
        if (stat(probe, &st) || !S_ISDIR(st.st_mode)) continue;
        size_t used = strlen(out);
        if (used + strlen(cand) + 2 >= n) break;
        if (used) out[used++] = ':';
        memcpy(out + used, cand, strlen(cand) + 1);
        DLOG("[romfs] 分包层 %{public}s", cand);
    }
    closedir(d);
}

static void *host_thread(void *p)
{
    (void)p;
    const char *dir = g_dir;
    if (!dir[0]) {
        DLOG("【红】start 未传入游戏文件目录");
        return NULL;
    }
    if (chdir(dir)) DLOG("【红】chdir %{public}s 失败", dir);

    char rp[1200], sp[1200], tp[1200];
    snprintf(sp, sizeof sp, "%s/save", dir);
    snprintf(tp, sizeof tp, "%s/temp", dir);
    mkdir(sp, 0770);
    mkdir(tp, 0770);
    // $: 挂到包内只读资源目录（资源全打进 HAP，就地读；沙箱里不留第二份）
    int hit = pick_romfs(g_res, dir, rp, sizeof rp);
    setenv("DM_ROMFS", rp, 1);
    // 其余模块的 resfile/romfs 作为覆盖层接上（档案拆包，游戏侧仍是完整 $:/data）
    if (hit) {
        char ov[3000] = "";
        romfs_overlay_from_bundle(ov, sizeof ov, rp);
        if (ov[0]) {
            const char *prev = getenv("DM_ROMFS_OVERLAY");
            char both[3200];
            snprintf(both, sizeof both, "%s%s%s", ov, prev && prev[0] ? ":" : "", prev ? prev : "");
            setenv("DM_ROMFS_OVERLAY", both, 1);
            DLOG("[romfs] 分包覆盖层 = %{public}s", both);
        }
    }
    setenv("DM_SAVE", sp, 1);
    setenv("DM_TEMP", tp, 1);
    if (!getenv("DM_TACT_PREFETCH")) setenv("DM_TACT_PREFETCH", "0", 1);   // 首测：不预取（无 bootlist.tsv）

    // 真机诊断开关默认打开（外部已设则不覆盖）：HLE 计数走 hilog、fs 打开路径日志、NVN 对象创建日志、50ms 采样客体主线程
    // Real-device diagnostic switches on by default (not overridden if set outside): HLE counts to hilog, fs open-path log, NVN object creation log, 50ms sampling of the guest main thread
    { char hp[1200]; snprintf(hp, sizeof hp, "%s/hlc.tsv", dir);
      if (!getenv("DM_HLECOUNT")) setenv("DM_HLECOUNT", hp, 1);
      if (!getenv("DM_HLECOUNT_LOG")) setenv("DM_HLECOUNT_LOG", "1", 1);
      if (!getenv("DM_FS")) setenv("DM_FS", "1", 1);
      // DM_THR 默认不开：它是 !!getenv 型的「设了就开」开关，svc 逐条日志在真机 12 秒能写 10 万行，
      // 把 hilog 环形缓冲挤爆、图形层自己的行全被顶掉；要查线程时用 --ps DM_THR 1 打开。
      // DM_THR stays unset by default: it is an "enabled if present" flag and per-svc logging writes 100k lines in 12 s on device,
      // which evicts the hilog ring buffer and the graphics layer's lines with it. Turn it on with --ps DM_THR 1 when chasing threads.
      if (!getenv("DM_NVN_LOG")) setenv("DM_NVN_LOG", "1", 1);
      if (!getenv("DM_SAMPLE")) setenv("DM_SAMPLE", "50:*", 1); }   // 全客体线程轮询采样 | Cycle-sample all guest threads

    redirect_output();
    DLOG("宿主启动：dir=%{public}s", dir);
    mmap_probe(dir);
    // 模块集可由 DM_MODULES（逗号/分号/空格分隔）给出；缺省 main,sdk —— 与旧版逐字一致，换游戏时才用
    // `aa start --ps DM_MODULES main,sdk,subsdk0,subsdk1` 传，已验收的 D2R 行为不变
    // The module set can be given by DM_MODULES (comma / semicolon / space separated); the default main,sdk is word-for-word the
    // old behaviour, so only a different game passes `aa start --ps DM_MODULES main,sdk,subsdk0,subsdk1`
    static char *mn[8]; static int mn_n = 0;
    { const char *me = getenv("DM_MODULES");
      if (me && *me) { static char mbuf[256]; snprintf(mbuf, sizeof mbuf, "%s", me);
        for (char *t = strtok(mbuf, ",; "); t && mn_n < 8; t = strtok(NULL, ",; ")) mn[mn_n++] = strdup(t); } }
    static char *argv[12]; int ac = 0;
    argv[ac++] = (char *)"D2R"; argv[ac++] = g_dir;
    for (int k = 0; k < mn_n; k++) argv[ac++] = mn[k];
    argv[ac] = NULL;
    int rc = main(ac, argv);
    DLOG("宿主 main 退出 rc=%{public}d", rc);

    // 诊断：递归列沙箱目录（files / save / romfs / temp 深 3）+ 读游戏自己写的日志（temp/blz-log.txt 前 300 行）
    // Diagnostic: recursively list the sandbox directories (files / save / romfs / temp, depth 3) + read the log the game itself wrote (first 300 lines of temp/blz-log.txt)
    static const char *subs[] = {"", "/save", "/romfs", "/temp"};
    for (int si = 0; si < 4; si++) {
        char dp[1300]; snprintf(dp, sizeof dp, "%s%s", dir, subs[si]);
        int budget = 80;
        list_rec(dp, 0, &budget);
    }
    { char lp[1300]; snprintf(lp, sizeof lp, "%s/temp/blz-log.txt", dir); dump_file_log(lp, 100); }
    { char gd[1300]; snprintf(gd, sizeof gd, "%s/temp/Diablo II/GameLogs", dir);
      DIR *d = opendir(gd);
      if (!d) DLOG("[dump] %{public}s 打不开", gd);
      else { struct dirent *e; while ((e = readdir(d))) {
          if (e->d_name[0] == '.') continue;
          char fp[1500]; snprintf(fp, sizeof fp, "%s/%s", gd, e->d_name);
          dump_file_log(fp, 300);
      } closedir(d); } }
    return NULL;
}

// dump 一个文本文件到 hilog（诊断）：每行一条，≤ max_lines 行
// Dump a text file to hilog (diagnostic): one entry per line, at most max_lines lines
static void dump_file_log(const char *path, int max_lines)
{
    FILE *f = fopen(path, "r");
    if (!f) { DLOG("[dump] %{public}s 打不开", path); return; }
    DLOG("[dump] %{public}s：", path);
    char line[1024]; int n = 0;
    while (fgets(line, sizeof line, f) && n < max_lines) {
        size_t l = strlen(line); while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (l) DLOG("  | %{public}s", line);
        n++;
    }
    fclose(f);
    if (n >= max_lines) DLOG("  …（截断到 %{public}d 行）", max_lines);
}

// 递归列目录（诊断，深度 ≤3、总数 ≤ budget）：FILE/DIR 各一行（带大小）
// Recursively list a directory (diagnostic, depth <= 3, at most budget entries): one line per FILE/DIR (with size)
static void list_rec(const char *root, int depth, int *budget)
{
    if (depth > 3 || *budget <= 0) return;
    DIR *d = opendir(root);
    if (!d) { DLOG("[ls] %{public}s 打不开", root); return; }
    struct dirent *e;
    while ((e = readdir(d)) && *budget > 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1500]; snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        DLOG("[ls] %{public}s%s  (%{public}lld B)", p, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size);
        (*budget)--;
        if (S_ISDIR(st.st_mode)) list_rec(p, depth + 1, budget);
    }
    closedir(d);
}

static napi_value Start(napi_env env, napi_callback_info info)
{
    napi_value ret = NULL;
    size_t argc = 3;
    napi_value argv[3] = {NULL, NULL, NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc >= 1 && argv[0]) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], g_dir, sizeof g_dir, &len);
    }
    // 第 2 个参数 = ArkTS 传下来的 Want.parameters JSON（`aa start --ps DM_xxx v` 可以从设备外设诊断开关）。
    // 只认 DM_ 开头的键，值为纯字符串；已经在宿主里设过的不覆盖（默认诊断块先跑，外部为准的是这里）。
    // Second argument = the Want.parameters JSON from ArkTS (`aa start --ps DM_xxx v` lets the diagnostic switches be set from outside the device).
    // Only keys starting with DM_ are taken, values plain strings.
    if (argc >= 2 && argv[1]) {
        // 2048 装不下：want.parameters 除了我们那十几条 DM_*，还被框架塞进 callerPid / callerToken / moduleName /
        // ohos.aafwk.param.* 等二十来条，实测整串 1 KB 出头，再加几条长路径开关就从尾部被无声截掉——
        // AJ0 轮的 DM_ARENA_ARM=0 就是这么丢的（日志回显「未设」），一整轮 A/B 白跑。截断现在自己报一条红。
        // 2048 does not fit: besides our dozen DM_* keys the framework also drops callerPid / callerToken / moduleName /
        // ohos.aafwk.param.* in there, so the string measured just over 1 KB, and a few extra long-path switches got cut
        // off the tail in silence - that is how round AJ0 lost DM_ARENA_ARM=0 (echo said 未设) and burned a whole A/B.
        // The truncation now reports itself.
        char js[16384];
        size_t len = 0;
        if (napi_get_value_string_utf8(env, argv[1], js, sizeof js, &len) == napi_ok) {
            if (len + 1 >= sizeof js)
                OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                             "[env] want 参数串被截（%{public}zu/%{public}zu），尾部的开关没落地", len, sizeof js);
            for (const char *p = js; (p = strstr(p, "\"DM_")) != NULL;) {
                const char *k = p + 1, *c = strchr(k, '"');
                if (!c) break;
                char key[64];
                size_t kl = (size_t)(c - k);
                if (kl >= sizeof key) { p = c; continue; }
                memcpy(key, k, kl);
                key[kl] = 0;
                p = c + 1;
                if (strncmp(p, ":\"", 2)) continue;
                p += 2;
                const char *v = p, *e = strchr(v, '"');
                if (!e) break;
                char val[512];
                size_t vl = (size_t)(e - v);
                if (vl >= sizeof val) vl = sizeof val - 1;
                memcpy(val, v, vl);
                val[vl] = 0;
                setenv(key, val, 1);
                OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "[env] %{public}s=%{public}s（外部传入）", key, val);
                p = e;
            }
        }
    }
    pad_ohos_env();     // 外部开关落定后才知道 DM_PAD 关不关手柄面（模块注册期还读不到 want 参数）
    // 第 3 个参数 = HAP 资源目录（ctx.resourceDir）。游戏数据打进安装包后，$: 就地挂到这里（见 pick_romfs）。
    // Third argument = the HAP resource directory (ctx.resourceDir). With the data inside the package, $: mounts there in place (see pick_romfs).
    { extern void nvn_wrap_arm(void); nvn_wrap_arm(); }   // NVN 包装层（DM_NVN_TRACE / DM_NVN_PROF）同样只能在外部开关落定后装 | the NVN wrapper layer can only be installed after the external switches have landed
    if (argc >= 3 && argv[2]) {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, argv[2], g_res, sizeof g_res, &len) == napi_ok) {
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "[romfs] resourceDir=%{public}s", g_res);
            setenv("DM_RES", g_res, 1);   // 重放层按它找 shaders.bin（绘制要用着色器库，M2-3b）| the replay layer finds shaders.bin through it (draws need the library, M2-3b)
        }
    }
    // 外部开关落定后再看要不要起 Vulkan 冒烟通条（表面多半已就绪）
    // 先把 stderr→hilog 桥架好：冒烟线程里驱动 abort 时 crash.c 的现场要能进 hilog
    redirect_output();
    vk_smoke_maybe_start();
    if (!g_started) {
        g_started = 1;
        const char *nh = getenv("DM_NOHOST");
        if (nh && nh[0] == '1') {
            // 隔离实验：只跑 Vulkan 冒烟，不起客体宿主（判断驱动 abort 是否与宿主地址空间有关）
            DLOG("DM_NOHOST=1：跳过宿主线程，只跑冒烟通条");
        } else {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 8 << 20);
        if (pthread_create(&t, &a, host_thread, NULL)) {
            g_started = 0;
            napi_create_int32(env, -1, &ret);
            return ret;
        }
        pthread_detach(t);
        }
    }
    napi_create_int32(env, 0, &ret);
    return ret;
}

static napi_value RegisterModule(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"start", NULL, Start, NULL, NULL, NULL, napi_default, NULL},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    // XComponent 用 libraryName 触发本次加载时，exports 里带 __NATIVE_XCOMPONENT_OBJ__ → 接表面回调
    xcomp_attach(env, exports);
    pad_ohos_register(env, exports);   // 手柄 / 触屏：padKey / padAxis / padTouch / padDiag 挂进 exports（pad_ohos.c） | controller / touch: attach padKey / padAxis / padTouch / padDiag to exports (pad_ohos.c)
    front_ohos_register(env, exports); // 软键盘：kbdPoll / kbdAnswer（front_ohos.c）| software keyboard: kbdPoll / kbdAnswer (front_ohos.c)
    return exports;
}

static napi_module dmModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = NULL,
    .nm_register_func = RegisterModule,
    .nm_modname = "dmhost",
    .nm_priv = ((void *)0),
    .reserved = { 0 },
};

__attribute__((constructor)) void RegisterDmhostModule(void)
{
    napi_module_register(&dmModule);
}
