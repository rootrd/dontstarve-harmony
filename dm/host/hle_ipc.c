// 假服务管理器（sm:）与通用 HIPC 成功应答。
// 宿主没有 Horizon 内核服务，但 SDK 拿不到「成功」的应答就自杀：B27 实测 118.051 s，连 "sm:" 的占位 svc
// 原样返回成功却不改报文 → 客户把栈残值当 Result 读 → nn::diag::AbortImpl → svcBreak → 宿主 _exit(42)。
// 这里按 CMIF/HIPC 报文格式，往这条线程的报文缓冲写「Result=0、无输出」的应答（输出参数一律读成 0），
// sm: 的取服务命令额外回一个假会话句柄；同时把「服务名 / 命令号 / 参数」打出来，作为后续按服务补真实现的依据。
// Fake service manager (sm:) plus a generic HIPC "success, no outputs" reply.
// The host has no Horizon kernel services, but the SDK aborts on anything but a successful reply: measured on B27 at
// 118.051 s -- the placeholder for connecting to "sm:" returned success without touching the message buffer, so the
// client read stack residue as its Result, ran into nn::diag::AbortImpl -> svcBreak -> host _exit(42).
// Here we write a CMIF reply (Result=0, no outputs; the guest's out-params therefore read as 0) into this thread's
// message buffer, plus a fake session handle for sm:'s get-service command, and log service name / command id /
// parameters so real per-service implementations can be added from the evidence.
#include "hle.h"

extern pthread_key_t g_keytlr;   // thread.c：客体的 Horizon ThreadLocalRegion；报文缓冲就在 TLR+0x000（0x100 字节）
                                 // the guest's Horizon ThreadLocalRegion; its message buffer sits at TLR+0x000, 0x100 bytes

#define SRV_MAX 96
#define SRV_NAME 12
typedef struct { char name[SRV_NAME]; uint32_t h; } srv_ent;
static srv_ent g_srv[SRV_MAX];
static unsigned g_nsrv;
static uint32_t g_hnext = 0xE0000000u;   // 假会话句柄段：与宿主自己发出去的句柄号隔开 | fake session handle range, kept away from the host's own handle numbers
static pthread_mutex_t g_ipc_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint32_t srv_new(const char *name) {
    uint32_t h = 0;
    pthread_mutex_lock(&g_ipc_mtx);
    for (unsigned i = 0; i < g_nsrv; i++) if (!strcmp(g_srv[i].name, name)) { h = g_srv[i].h; goto out; }
    if (g_nsrv < SRV_MAX) {
        snprintf(g_srv[g_nsrv].name, SRV_NAME, "%s", name);
        g_srv[g_nsrv].h = h = g_hnext++;
    }
out:
    pthread_mutex_unlock(&g_ipc_mtx);
    return h;
}

static int srv_of(uint32_t h, char out[SRV_NAME]) {
    int ok = 0;
    pthread_mutex_lock(&g_ipc_mtx);
    for (unsigned i = 0; i < g_nsrv; i++) if (g_srv[i].h == h) { memcpy(out, g_srv[i].name, SRV_NAME); ok = 1; break; }
    pthread_mutex_unlock(&g_ipc_mtx);
    if (!ok) out[0] = 0;
    return ok;
}

// 一次请求的报文头是 HIPC2（不是老 CMIF）。口径从客体 sdk 自己的解析代码反推出来（三条都是它 .dynsym 里的函数）：
//   sdk+0x5edc  nn::sf::hipc::detail::HipcMessageReader::Initialize(const void*)
//   sdk+0x37f3c4 nn::sf::hipc::detail::HipcMessageBufferAccessor::ParseHeader(...)
//   sdk+0x37f4b8 一组 12 字节访问器（SetTag/GetTag/SetProcessId/GetCopyHandle/GetMoveHandle/SetPointer/SetSend/SetReceive/SetExchange）
// w[0] = 命令号(16) | 消息类型(4)@16 | 定长描述符数(4)@20 | 变长描述符数(4)@24 | 自动缓冲数(4)@28
// w[1] = 数据字数(10) | 自动缓冲列表模式(4)@10（0 无 / 1 = 4 字节项 / 2 = 8 字节项）| 列表偏移(11)@14 | 有特殊头(1)@31
//   数据区起点 = (w[1] & 0x3ff) * 4 再 & ~3，紧跟在头部＋描述符之后；有特殊头时 w[2] = 标记(1)@0 | copy 句柄数(4)@1 | move 句柄数(4)@5，
//   其后依次 copy 句柄(每个 4B)、move 句柄(每个 4B)、定长描述符（每个 8B：w0 = 大小@0-11 | 模式@12-15 | 索引@16-19 …，w1 = 地址低 32 位）。
//   发送/接收/交换缓冲描述符每个 3 个字（地址 8B + 大小/模式 4B），ReceiveList 每项 8B。
// 应答与请求同一种头：Result 是第一个数据字。字段名沿用老变量，别再当成 CMIF 的「输入静态缓冲/发送缓冲」读。
// The request header is HIPC2, not classic CMIF. Decoded from the guest's own parsers (sdk+0x5edc HipcMessageReader::Initialize,
// sdk+0x37f3c4 HipcMessageBufferAccessor::ParseHeader, and the 12-byte accessors at sdk+0x37f4b8):
// w[0] = cmd(16) | msg type(4) | fixed-desc count(4) | variant-desc count(4) | auto-buf count(4);
// w[1] = data words(10) | auto-list mode(4)@10 (0 none / 1 = 4-byte items / 2 = 8-byte) | list offset(11)@14 | has-special(1)@31.
// The data region begins right after header+descriptors, sized (w[1]&0x3ff)*4 masked to ~3; the reply's Result is data[0].
// The old field names below are kept for the log, but they mean fixed/variant/auto, not "input static / send / recv / exchange buffers".
static struct { unsigned cmd, sstatic, sbuf, rbuf, xbuf, dw, rsm, copy, move; const uint32_t *w, *d; } rq;

static void rq_parse(uint32_t *w) {
    rq.w = w;
    rq.cmd = w[0] & 0xffff;
    rq.sstatic = (w[0] >> 16) & 0xf; rq.sbuf = (w[0] >> 20) & 0xf;
    rq.rbuf = (w[0] >> 24) & 0xf;   rq.xbuf = (w[0] >> 28) & 0xf;
    rq.dw = w[1] & 0x3ff; rq.rsm = (w[1] >> 10) & 0xf;
    rq.copy = rq.move = 0;
    const uint32_t *p = w + 2;
    if ((w[1] >> 31) & 1) {                              // 有特殊头 | special header present
        rq.copy = (w[2] >> 1) & 0xf; rq.move = (w[2] >> 5) & 0xf;
        p = w + 3;
        if (w[2] & 1) p += 2;                             // 跟 8 字节 pid | followed by the 8-byte pid
    }
    p += rq.copy + rq.move + rq.sstatic * 2;
    if (p > w + 0x40 - 4) p = w + 0x40 - 4;   // 报文只有 0x100 字节：头部残值把指针算飞了就夹回来 | the buffer is 0x100 bytes; clamp a header-garbage pointer
    rq.d = p;
}

// 数据字里前两个若是一串可打印 ASCII，就是内联的服务名（sm 的 ServiceName 是 8 字节内联参数）。
// If the first two data words are printable ASCII, they are the inline service name (sm's ServiceName is an inline u64).
static void rq_name(char out[SRV_NAME]) {
    const uint32_t *d = rq.d;
    char t[8];
    memcpy(t, d, 8);
    size_t k = 0;
    while (k < 8 && t[k] >= 0x20 && t[k] < 0x7f) { out[k] = t[k]; k++; }
    out[k] = 0;
}

// 应答：Result=0，可选一个 move 句柄；其余报文区清零 ⇒ 客体的输出参数读到 0。
// Reply: Result=0 with an optional move handle; the rest of the buffer is zeroed, so the guest's out-params read 0.
static void rp_write(uint32_t *w, uint32_t handle) {
    memset(w, 0, 0x100);
    w[0] = 0;                       // 响应头低半：特殊/对象 id 增量 = 0，输出静态缓冲 = 0 | response header low half
    w[1] = 1u | (handle ? (1u << 31) : 0);   // 1 个数据字（Result）+ 有无特殊头 | one data word (the Result) plus the special-header bit
    if (handle) { w[2] = 1u << 5; w[3] = handle; w[4] = 0; }   // 特殊头(1 个 move 句柄) → 句柄 → Result
}

// 每个（服务, 命令）只详打第一次，之后按计数抑制 —— hid/音频一类服务会被每帧轮询，全打会把 hilog 冲爆。
// Print each (service, command) pair in full once, then suppress with a counter -- hid/audio-style services get polled
// every frame and would flood hilog.
static void ipc_note(const char *svc, unsigned cmd, const char *line) {
    static struct { char k[24]; unsigned n; } seen[64];
    static unsigned nseen;
    char k[24];
    snprintf(k, sizeof k, "%s:%u", svc, cmd);
    int hit = -1;
    for (unsigned i = 0; i < nseen; i++) if (!strcmp(seen[i].k, k)) { hit = (int)i; break; }
    if (hit < 0) {
        if (nseen < 64) { snprintf(seen[nseen].k, sizeof seen[nseen].k, "%s", k); seen[nseen].n = 1; nseen++; }
        LOG("%s", line);
        return;
    }
    if (!(++seen[hit].n & 0x3ff)) LOG("%s（同类已 %u 次）", line, seen[hit].n);
}

void dm_ipc_connect(uint64_t *regs) {
    // svcConnectToNamedPort(u32 *out_handle, const char *name)：B27 实测名字在 x1、输出句柄指针在 x0。
    // Measured on B27: the name is in x1 and the out-handle pointer in x0.
    const char *q = (const char *)(uintptr_t)regs[1];
    char nm[SRV_NAME] = {0};
    if (q) { size_t k = 0; while (k < SRV_NAME - 1 && q[k] >= 0x20 && q[k] < 0x7f) { nm[k] = q[k]; k++; } }
    if (!nm[0]) { regs[0] = 0xE801; return; }             // NotFound（真机同名口径）| same shape the real device uses
    uint32_t h = srv_new(nm);
    uint32_t *out = (uint32_t *)(uintptr_t)regs[0];
    if (out) *out = h;
    regs[1] = h;   // 两种口径都给：真内核写穿 x0 指针（B27 实测客户确实从那个栈槽读回了残值），而宿主别处
                   // 有按"出参留在原寄存器里"回值的先例（SetHeapSize 回 x1）——多写一个寄存器不影响任何一侧
                   // Do both: the real kernel writes through the x0 pointer (B27 shows the client read back stack residue
                   // from that slot), while this host elsewhere returns out-params in the register they were passed in
                   // (SetHeapSize answers in x1). An extra register write is harmless to either side.
    // B29 判据：写完立刻读回那个槽，确认客体之后是从指针取的（*out=h 生效）还是根本没走指针这条路。
    // B29's test: read the slot straight back to confirm the guest takes the handle through the pointer (the *out = h
    // write sticks) or never used the pointer at all.
    LOG("IPC 连服务 %s -> 假句柄 %#x（出指针 %#llx，回读=%#x，x1=%#llx）", nm, h, (unsigned long long)regs[0],
        out ? *out : 0u, (unsigned long long)regs[1]);
    regs[0] = 0;
}

void dm_ipc_send(uint64_t *regs) {
    uint32_t h = (uint32_t)regs[0];
    uint32_t *w = (uint32_t *)pthread_getspecific(g_keytlr);   // TLR 基址 = 报文缓冲 | TLR base = the message buffer
    if (!w) { regs[0] = 0; return; }
    rq_parse(w);
    char svc[SRV_NAME];
    if (!srv_of(h, svc)) snprintf(svc, sizeof svc, "?%#x", h);
    char nm[SRV_NAME];
    rq_name(nm);
    char line[256];
    snprintf(line, sizeof line,
             "IPC 请求 服务=%s 句柄=%#x cmd=%u 类%u 定%u 变%u 自%u 数据%u 模式%u 特殊%u copy%u move%u 参数=%#x %#x %#x %#x（x0=%#llx x1=%#llx x2=%#llx x3=%#llx）",
             svc, h, rq.cmd, rq.sstatic, rq.sbuf, rq.rbuf, rq.xbuf, rq.dw, rq.rsm, rq.copy, rq.move,
             rq.d[0], rq.d[1], rq.d[2], rq.d[3],
             (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2], (unsigned long long)regs[3]);
    ipc_note(svc, rq.cmd, line);

    // B29 判据：报文缓冲到底在 TLR 的哪一段。前若干条把整块 0x200 原样导出来（8 段 × 16 字），
    // 离线一眼看出请求头在 +0x000 还是 +0x100、数据字接着哪里 —— 猜错一处，应答就写进了客体不看的地方。
    // B29's test: which part of the TLR really holds the message buffer. Dump the whole 0x200 block for the first few
    // requests so the header's offset (+0x000 or +0x100) and where the data words follow are readable offline -- guess
    // it wrong and the reply lands where the guest never looks.
    { static unsigned ndump;
      if (ndump++ < 6) {
          for (int seg = 0; seg < 8; seg++) {
              char b[200]; int o = 0;
              for (int i = 0; i < 16; i++) o += snprintf(b + o, sizeof b - o, " %08x", w[seg * 16 + i]);
              LOG("IPC 报文[%u]@%#x:%s", ndump - 1, seg * 0x40, b);
          }
      } }

    // sm:（服务管理器）：cmd 1 = GetServiceHandle、cmd 2 = RegisterService，二者都要回一个会话句柄；
    // cmd 0 = RegisterClient/Initialize，只回 Result。其余服务先统一「成功、无输出」。
    // sm: -- cmd 1 GetServiceHandle and cmd 2 RegisterService both return a session handle; cmd 0 (RegisterClient /
    // Initialize) only the Result. Other services get the generic "success, no outputs" for now.
    uint32_t out_h = 0;
    if (!strcmp(svc, "sm") && (rq.cmd == 1 || rq.cmd == 2)) {
        out_h = srv_new(nm);
        LOG("IPC sm 要服务 %s -> 假句柄 %#x", nm, out_h);
    }
    rp_write(w, out_h);
    regs[0] = 0;
}
