// 客体的 stdio 接到宿主：把 main.so 导入的 fopen/fread/… 整套换成宿主实现。
// 为什么：main.so 的 stdio 解析到 sdk.so 自带的 nnmusl（Nintendo 的 musl），它的开档要经 IPC svc（门表里登记的 112/114），
// 而实测崩溃时的 HLE 计数里 svc 最大只到 0x45，这两条一次都没发过 —— 也就是 nnmusl 在本宿主里走不到服务层，
// "rom:/shaders_nx/font.ksh" 一律打不开。客体随后打印「Failed to load shader: font」，着色器对象留空，
// 下一次取 [this+0x58] 就是那个 addr=0x138 的 SIGSEGV。
// 接管规则：凡是拿 FILE* 的函数必须一起接（漏一个，客体 musl 就会去解宿主 FILE 的函数指针）；
// 路径先过挂载表翻译（和 nn::fs HLE 同一套：rom:/save:/temp:/… 加 ds2 分包覆盖层）；
// 不含 ':' 的路径（客体自己拼的相对路径）原样交给宿主，别把它挡掉。
// fd 只认我们自己发出去的号：客体的 socket fd 也从小整数往上排，拿宿主 close() 去关它就关掉了 Vulkan / 音频的宿主句柄，
// 所以 g_my_fd 之外的一律按 EBADF 拒掉（顶多漏一个关不掉的 socket，不伤宿主资源）。
#include "hle.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>   // f_own 的 __atomic_compare_exchange_n 用了 false | f_own passes false to __atomic_compare_exchange_n
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_fs_stdio_map(const char *guest, char *out, size_t n, int create);   // hle_fs.c

enum { MAP_NO = 0, MAP_HIT = 1, MAP_RAW = 2 };
static _Atomic unsigned g_o_req, g_o_ok, g_o_no;
static unsigned char g_my_fd[65536];
static void fd_own(int fd) { if (fd >= 0 && fd < (int)sizeof g_my_fd) g_my_fd[fd] = 1; }
static int fd_ours(int fd) { return fd >= 0 && fd < (int)sizeof g_my_fd && g_my_fd[fd]; }
static void fd_drop(int fd) { if (fd >= 0 && fd < (int)sizeof g_my_fd) g_my_fd[fd] = 0; }

// FILE* 只认我们自己发出去的：客体内置的 stdin/stdout/stderr 是 nnmusl 自己的 FILE，
// 拿宿主 fclose/fileno 去动它就是解错结构体（顶多把宿主 stdio 搅坏）。凡是收到「不是我们的」FILE*，
// 一律当成往控制台写/读：写 → 镜像进 hilog（客体的 printf 文本本来一条也看不见，FMOD 的报错就是这么丢的），
// 读 → 直接 EOF。登记表只存 fopen/fdopen/freopen 的返回值，量很小（实测几十条），线性找够用。
#define MAX_MY_FILE 4096
static FILE *g_my_file[MAX_MY_FILE];
static _Atomic unsigned g_my_file_cur;
static unsigned my_file_slots(void) { unsigned n = atomic_load_explicit(&g_my_file_cur, memory_order_acquire); return n < MAX_MY_FILE ? n : MAX_MY_FILE; }
// 找一个空槽登记（指针用 __atomic 内建，普通数组元素也能原子交换）；槽满（同时开着 >4096 个文件，不会发生）
// 就退回旧行为：不记，反正顶多多镜像一条文本
static void f_own(FILE *f) {
    if (!f) return;
    unsigned start = atomic_fetch_add(&g_my_file_cur, 1);
    for (unsigned k = 0; k < MAX_MY_FILE; k++) {
        unsigned i = (start + k) % MAX_MY_FILE;
        FILE *want = NULL;
        if (__atomic_compare_exchange_n(&g_my_file[i], &want, f, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) return;
    }
    LOG("【红】stdio FILE 登记表满了（同时打开的文件超过 %d 个）", MAX_MY_FILE);
}
static void f_drop(FILE *f) {
    for (unsigned i = 0, n = my_file_slots(); i < n; i++)
        if (__atomic_load_n(&g_my_file[i], __ATOMIC_ACQUIRE) == f) { __atomic_store_n(&g_my_file[i], (FILE *)0, __ATOMIC_RELEASE); return; }
}
static int f_ours(FILE *f) {
    if (!f) return 0;
    for (unsigned i = 0, n = my_file_slots(); i < n; i++)
        if (__atomic_load_n(&g_my_file[i], __ATOMIC_ACQUIRE) == f) return 1;
    return 0;
}

#define minlen(a,b) ((a)<(b)?(a):(b))
static _Atomic unsigned g_out_n;
// 按行喂给 LOG：hilog 一条记录里带换行会和别的线程交错，拆开打才读得动
static void out_write(const char *b, size_t n) {
    if (g_out_n++ >= 20000) return;
    char line[1024]; size_t i = 0;
    while (i < n) {
        size_t start = i, j = i;
        while (j < n && b[j] != '\n' && j - start < sizeof line - 1) j++;
        size_t k = j - start;
        if (k) { memcpy(line, b + start, k); line[k] = 0; LOG("客体输出: %s", line); }
        i = j + 1;
        if (j == n) break;
    }
}
static void out_str(const char *t) { if (t) out_write(t, strlen(t)); }

// 翻译 + 计数；命中返回 1 并写 out，无前缀返回 2（*use 指向原串），彻底没命中返回 0
static int stdio_xlate(const char *p, char *out, size_t n, int create, const char **use) {
    if (!p) { errno = ENOENT; return MAP_NO; }
    if (dm_fs_stdio_map(p, out, n, create)) { *use = out; return MAP_HIT; }
    if (!strchr(p, ':')) { *use = p; return MAP_RAW; }
    errno = ENOENT; *use = p; return MAP_NO;
}
static int log_this(unsigned k, const char *p) { return k < 80 || (p && strstr(p, "shader")); }

HLE_FORCE("fopen", "nnmusl 开档走不到服务层；stdio 整体接宿主", FILE *, h_fopen, const char *p, const char *m) {
    char hp[1128]; const char *use;
    int t = stdio_xlate(p, hp, sizeof hp, (m && strpbrk(m, "waxW+")) ? 1 : 0, &use);
    FILE *f = fopen(use, m);
    (void)atomic_fetch_add(&g_o_req, 1);
    if (f) { unsigned j = atomic_fetch_add(&g_o_ok, 1); fd_own(fileno(f)); f_own(f);
             if (log_this(j, p)) LOG("stdio fopen#%u %s → %s（%s）", j + 1, p, use, t == MAP_HIT ? "挂载" : "原样"); }
    else { unsigned j = atomic_fetch_add(&g_o_no, 1); if (log_this(j, p)) LOG("【红】stdio fopen#%u 打不开 %s（翻译=%d errno=%d）", j + 1, p, t, errno); }
    return f;
}
HLE_FORCE("fdopen", "宿主 fd 配宿主 FILE*", FILE *, h_fdopen, int fd, const char *m) {
    FILE *f = fd_ours(fd) ? fdopen(fd, m) : (errno = EBADF, NULL);
    if (f) { fd_own(fileno(f)); f_own(f); }
    return f;
}
HLE_FORCE("freopen", "同上", FILE *, h_freopen, const char *p, const char *m, FILE *f) {
    char hp[1128]; const char *use; stdio_xlate(p, hp, sizeof hp, 1, &use);
    if (!f_ours(f)) { return f; }   // 客体想把 nnmusl 的 stdout 改挂到文件：不接，保住宿主 stdio
    FILE *r = freopen(use, m, f); if (r) { fd_own(fileno(r)); f_own(r); } return r;
}
HLE_FORCE("fclose", "我们的 FILE* 才关；客体 stdout/stderr 交回来直接成功，别去关宿主 stdio", int, h_fclose, FILE *f) {
    if (!f_ours(f)) return 0;
    int fd = f ? fileno(f) : -1; int r = fclose(f); fd_drop(fd); f_drop(f); return r;
}
HLE_FORCE("fflush", "同上", int, h_fflush, FILE *f) {
    if (f && !f_ours(f)) return 0;
    return fflush(f);
}
HLE_FORCE("feof", "外来 FILE 当读到末尾", int, h_feof, FILE *f) { return !f_ours(f) ? 1 : feof(f); }
HLE_FORCE("ferror", "外来 FILE 不算错", int, h_ferror, FILE *f) { return f_ours(f) ? ferror(f) : 0; }
HLE_FORCE("clearerr", "同上", void, h_clearerr, FILE *f) { if (f_ours(f)) clearerr(f); }
HLE_FORCE("fread", "外来 FILE 没有输入，直接 EOF", size_t, h_fread, void *b, size_t s_, size_t c, FILE *f) {
    return f_ours(f) ? fread(b, s_, c, f) : 0;
}
HLE_FORCE("fwrite", "外来 FILE = 控制台：字节镜像进 hilog", size_t, h_fwrite, const void *b, size_t s_, size_t c, FILE *f) {
    if (!f_ours(f)) { out_write((const char *)b, s_ * c); return s_ * c; }
    return fwrite(b, s_, c, f);
}
HLE_FORCE("fgets", "外来 FILE 没有输入", char *, h_fgets, char *b, int n, FILE *f) { return f_ours(f) ? fgets(b, n, f) : NULL; }
HLE_FORCE("fputs", "同 fwrite", int, h_fputs, const char *str, FILE *f) {
    if (!f_ours(f)) { out_str(str); return 0; }
    return fputs(str, f);
}
HLE_FORCE("fputc", "同 fwrite", int, h_fputc, int c, FILE *f) {
    if (!f_ours(f)) { char t = (char)c; out_write(&t, 1); return (unsigned char)c; }
    return fputc(c, f);
}
HLE_FORCE("fgetc", "外来 FILE 没有输入", int, h_fgetc, FILE *f) { return f_ours(f) ? fgetc(f) : EOF; }
HLE_FORCE("getc", "同上", int, h_getc, FILE *f) { return f_ours(f) ? fgetc(f) : EOF; }
HLE_FORCE("putc", "同 fputc", int, h_putc, int c, FILE *f) {
    if (!f_ours(f)) { char t = (char)c; out_write(&t, 1); return (unsigned char)c; }
    return fputc(c, f);
}
HLE_FORCE("ungetc", "外来 FILE 没得推", int, h_ungetc, int c, FILE *f) { return f_ours(f) ? ungetc(c, f) : EOF; }
HLE_FORCE("fseek", "外来 FILE 不动", int, h_fseek, FILE *f, long off, int wh) { return f_ours(f) ? fseek(f, off, wh) : -1; }
HLE_FORCE("fseeko", "同上", int, h_fseeko, FILE *f, off_t off, int wh) { return f_ours(f) ? fseeko(f, off, wh) : -1; }
HLE_FORCE("ftell", "外来 FILE 报 0，别让客体以为读到了负数", long, h_ftell, FILE *f) { return f_ours(f) ? ftell(f) : 0; }
HLE_FORCE("ftello", "同上", off_t, h_ftello, FILE *f) { return f_ours(f) ? ftello(f) : 0; }
HLE_FORCE("setvbuf", "外来 FILE 的缓冲区不是我们的结构，跳过", int, h_setvbuf, FILE *f, char *b, int m, size_t n) {
    return f_ours(f) ? setvbuf(f, b, m, n) : 0;
}
HLE_FORCE("fileno", "外来 FILE 给不出宿主 fd", int, h_fileno, FILE *f) {
    if (!f_ours(f)) return errno = EBADF, -1;
    return fileno(f);
}
HLE_FORCE("fprintf", "拿宿主 FILE*，变参只能走 vfprintf（>8 个参数时 va 追不到客体栈，实测日志够用）", int, h_fprintf, FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r;
    if (!f_ours(f)) { char buf[2048]; r = vsnprintf(buf, sizeof buf, fmt, ap); out_write(buf, r > 0 ? minlen(r, (int)sizeof buf - 1) : 0); }
    else r = vfprintf(f, fmt, ap);
    va_end(ap); return r;
}
HLE_FORCE("fscanf", "外来 FILE 没有输入", int, h_fscanf, FILE *f, const char *fmt, ...) {
    if (!f_ours(f)) return EOF;
    va_list ap; va_start(ap, fmt); int r = vfscanf(f, fmt, ap); va_end(ap); return r;
}
// 控制台三件套：客体 main.so 的 printf/puts 是导入符号（解析到 sdk 的 nnmusl），写出去落在宿主 stdout，
// HAP 里没人收 —— 全部镜像进 hilog。FMOD 的错误（fmod_eventi.cpp 的行号）就走这条路。
// 只接带 "..." 的入口、变参用我们自己的 va_start 现攒；客体没有导入 vprintf，也就不用把它的 va_list
// （musl 的结构）交给宿主 vsnprintf（bionic 的结构），少赌一层 ABI。
HLE_FORCE("printf", "格式化后整行进 hilog", int, h_printf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); char buf[2048];
    int r = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    out_write(buf, r > 0 ? minlen(r, (int)sizeof buf - 1) : 0);
    return r;
}
HLE_FORCE("puts", "同上", int, h_puts, const char *str) { out_str(str); out_write("\n", 1); return 0; }
HLE_FORCE("putchar", "同上", int, h_putchar, int c) { char t = (char)c; out_write(&t, 1); return (unsigned char)c; }
HLE_FORCE("perror", "errno 也一并打出来", void, h_perror, const char *str) {
    char buf[512]; int n = snprintf(buf, sizeof buf, "客体 perror: %s: %s", str ? str : "", strerror(errno));
    out_write(buf, minlen(n, (int)sizeof buf - 1));
}

HLE_FORCE("open", "nnmusl 的 open 同上；只发我们自己的 fd", int, h_open, const char *p, int fl, ...) {
    mode_t md = 0;
    if (fl & O_CREAT) { va_list ap; va_start(ap, fl); md = (mode_t)va_arg(ap, int); va_end(ap); }
    char hp[1128]; const char *use; int t = stdio_xlate(p, hp, sizeof hp, (fl & O_CREAT) ? 1 : 0, &use);
    int fd = open(use, fl, md);
    unsigned k = atomic_fetch_add(&g_o_req, 1);
    if (fd >= 0) { fd_own(fd); if (log_this(k, p)) LOG("stdio open#%u %s → %s", k + 1, p, use); }
    else if (log_this(k, p)) LOG("【红】stdio open#%u 打不开 %s（翻译=%d errno=%d）", k + 1, p, t, errno);
    return fd;
}
HLE_FORCE("close", "只关我们发出去的 fd（客体的 socket fd 号会撞上宿主的）", int, h_close, int fd) {
    if (!fd_ours(fd)) { unsigned k = atomic_fetch_add(&g_o_no, 1); if (k < 40) LOG("stdio close 拒接非宿主 fd %d（第 %u 次）", fd, k + 1); return errno = EBADF, -1; }
    fd_drop(fd); return close(fd);
}
HLE_FORCE("stat", "宿主 musl 和 nnmusl 的 struct stat 同布局（都是 aarch64 musl）", int, h_stat, const char *p, struct stat *st) {
    char hp[1128]; const char *use; stdio_xlate(p, hp, sizeof hp, 0, &use); return stat(use, st);
}
HLE_FORCE("lstat", "同 stat", int, h_lstat, const char *p, struct stat *st) {
    char hp[1128]; const char *use; stdio_xlate(p, hp, sizeof hp, 0, &use); return lstat(use, st);
}
HLE_FORCE("fstat", "同 close：只认自己的 fd", int, h_fstat, int fd, struct stat *st) {
    if (!fd_ours(fd)) return errno = EBADF, -1;
    return fstat(fd, st);
}
HLE_FORCE("remove", "路径翻译后交宿主", int, h_remove, const char *p) {
    char hp[1128]; const char *use; stdio_xlate(p, hp, sizeof hp, 1, &use); return remove(use);
}
HLE_FORCE("rename", "两条路径都翻译", int, h_rename, const char *a, const char *b) {
    char ha[1128], hb[1128]; const char *ua, *ub;
    stdio_xlate(a, ha, sizeof ha, 1, &ua); stdio_xlate(b, hb, sizeof hb, 1, &ub);
    return rename(ua, ub);
}
