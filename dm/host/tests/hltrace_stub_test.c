// DM_HLTRACE 计数桩形状的本机自测（构建机是 aarch64 Ubuntu，能直接跑）。
// 覆盖三件事：8 个整型实参原样送达、8 个浮点实参不被记录函数踩坏、结构体返回指针 x8 保住；
// 顺带核对计数累加与「记到的 x0..x2」。桩的模板必须与 hle_count.c 里那份逐字一致（那边改这里就得改）。
// Native self-test of the DM_HLTRACE stub shape (the build box is aarch64 Ubuntu, so it runs directly).
// Covers: 8 integer arguments arrive untouched, 8 double arguments are not clobbered by the recorder, the struct-return
// pointer x8 survives; also checks the counter and the recorded x0..x2. The template below must stay word-for-word
// identical to the one in hle_count.c.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define HC_MAX 64
#define TR_STRIDE 56
typedef struct hc_ent { const char *kind, *name; void *fn; _Atomic uint64_t n; } hc_ent;
static hc_ent g_hc[HC_MAX]; static unsigned g_nhc; static uint32_t *g_trcode;

static const char *g_last_name; static uint64_t g_last_a[3];
static void hc_trace_call(const uint64_t *a, hc_ent *e) {
    atomic_fetch_add(&e->n, 1);
    g_last_name = e->name; g_last_a[0] = a[0]; g_last_a[1] = a[1]; g_last_a[2] = a[2];
}
static void *trace_stub(unsigned i, void *fn) {
    static const uint32_t tmpl[] = {
        0xD10343FF,
        0xF90003E0, 0xF90007E1, 0xF9000BE2, 0xF9000FE3,
        0xF90013E4, 0xF90017E5, 0xF9001BE6, 0xF9001FE7,
        0xF90023E8, 0xF90027FE,
        0x3D8017E0, 0x3D801BE1, 0x3D801FE2, 0x3D8023E3,
        0x3D8027E4, 0x3D802BE5, 0x3D802FE6, 0x3D8033E7,
        0x910003E0,
        0x58000350,
        0xAA1003E1,
        0x58000350,
        0xD63F0200,
        0xF94027FE,
        0xF94003E0, 0xF94007E1, 0xF9400BE2, 0xF9400FE3,
        0xF94013E4, 0xF94017E5, 0xF9401BE6, 0xF9401FE7,
        0xF94023E8,
        0x3DC017E0, 0x3DC01BE1, 0x3DC01FE2, 0x3DC023E3,
        0x3DC027E4, 0x3DC02BE5, 0x3DC02FE6, 0x3DC033E7,
        0x910343FF,
        0x580000F0,
        0xD61F0200,
        0xD503201F,
    };
    uint32_t *w = g_trcode + TR_STRIDE * i;
    memcpy(w, tmpl, sizeof tmpl);
    uint64_t v = (uint64_t)&g_hc[i]; memcpy(w + 46, &v, 8);
    v = (uint64_t)&hc_trace_call;    memcpy(w + 48, &v, 8);
    v = (uint64_t)fn;                memcpy(w + 50, &v, 8);
    __builtin___clear_cache((char *)w, (char *)w + TR_STRIDE * 4);
    return w;
}
static unsigned reg(const char *name, void *fn) {
    unsigned i = g_nhc++; g_hc[i].kind = "TEST"; g_hc[i].name = name; g_hc[i].fn = fn;
    atomic_store(&g_hc[i].n, 0); return i;
}

static long sum8(long a, long b, long c, long d, long e, long f, long g, long h) {
    return a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
}
static double dsum8(double a, double b, double c, double d, double e, double f, double g, double h) {
    return a + b + c + d + e + f + g + h;
}
typedef struct { long x, y; } pair_t;
static pair_t sret2(long k) { pair_t p = { k * 3, k * 7 }; return p; }
static long takes_ptr(long p0, long p1, long p2) { return p0 + p1 * 2 + p2 * 3; }
static long nine(long a, long b, long c, long d, long e, long f, long g, long h, long i) {
    return a + b + c + d + e + f + g + h + i;
}

static int fails;
static void chk(const char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %ld\n", what, got);
    else { printf("  【红】%s 得 %ld 应 %ld\n", what, got, want); fails++; }
}

int main(void) {
    g_trcode = mmap(NULL, HC_MAX * TR_STRIDE * 4, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_trcode == MAP_FAILED) { perror("mmap"); return 2; }

    unsigned i0 = reg("sum8", (void *)sum8);
    long (*c0)(long, long, long, long, long, long, long, long) = trace_stub(i0, (void *)sum8);
    chk("8 个整型参（经桩）", c0(1, 2, 3, 4, 5, 6, 7, 8), 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64);
    chk("桩里计到的次数", (long)atomic_load(&g_hc[i0].n), 1);
    chk("记到的 x0", (long)g_last_a[0], 1);
    chk("记到的 x1", (long)g_last_a[1], 2);
    chk("记到的 x2", (long)g_last_a[2], 3);

    unsigned i1 = reg("dsum8", (void *)dsum8);
    double (*c1)(double, double, double, double, double, double, double, double) = trace_stub(i1, (void *)dsum8);
    chk("8 个浮点参（v0-v7 没被踩）", (long)(c1(1, 2, 3, 4, 8, 16, 32, 64) * 1000), 130000);

    unsigned i2 = reg("sret2", (void *)sret2);
    pair_t (*c2)(long) = trace_stub(i2, (void *)sret2);
    pair_t p = c2(5);
    chk("结构体返回 .x（x8 保住）", p.x, 15);
    chk("结构体返回 .y", p.y, 35);

    unsigned i3 = reg("takes_ptr", (void *)takes_ptr);
    long arr[4] = { 10, 20, 30, 40 };
    long (*c3)(long, long, long) = trace_stub(i3, (void *)takes_ptr);
    chk("指针实参照旧", c3((long)arr, (long)&arr[1], (long)&arr[2]),
        (long)arr + 2 * (long)&arr[1] + 3 * (long)&arr[2]);
    chk("记到的第三个参数是原指针", (long)g_last_a[2], (long)&arr[2]);

    unsigned i4 = reg("nine", (void *)nine);   // 第 9 个参在栈上：桩压了 208 字节，被调方读栈不能错位
    long (*c4)(long, long, long, long, long, long, long, long, long) = trace_stub(i4, (void *)nine);
    chk("第 9 个栈上参（sp 复原）", c4(1, 1, 1, 1, 1, 1, 1, 1, 100), 108);
    chk("连打两次计数", (long)(atomic_load(&g_hc[i4].n)), 1);
    c4(1, 1, 1, 1, 1, 1, 1, 1, 100);
    chk("连打两次计数", (long)(atomic_load(&g_hc[i4].n)), 2);

    printf(fails ? "【红】DM_HLTRACE 桩自测失败 %d 项\n" : "DM_HLTRACE 桩自测全通过\n", fails);
    return fails != 0;
}
