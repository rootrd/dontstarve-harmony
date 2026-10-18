// nn::account：真机经 acc:u0 服务管理用户；宿主固定一个本地用户。
// 联网账号（网络服务账号 / 任天堂账号授权 / 令牌）属排除范围，统一回 RES_FAIL，异步上下文的完成事件立即触发。
// 环境开关：DM_NICK=昵称（默认 Player）。
// nn::account: the real device manages users via the acc:u0 service; the host has one fixed local user.
// Online accounts (network service account / Nintendo Account authorization / tokens) are out of scope and uniformly return RES_FAIL; the async context's completion event fires immediately.
// Environment switch: DM_NICK=nickname (default Player).
#include "hle.h"

typedef struct { uint64_t v[2]; } Uid;                 // nn::account::Uid，16 字节 | nn::account::Uid, 16 bytes
typedef struct { uint64_t v[3]; } UserHandle;          // nn::account::UserHandle，24 字节 | nn::account::UserHandle, 24 bytes
_Static_assert(sizeof(Uid) == 16, "Uid");
static const Uid k_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // 非零即有效 | Non-zero means valid
#define HANDLE_MAGIC 0x55534552484e444cull                                    // "USERHNDL"

HLE_FORCE("_ZN2nn7account10InitializeEv", "连 acc:u0 账号服务；宿主固定本地用户", void, h_acc_Init, void) {}

HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "已打开用户列表；恒为本地用户 1 个", Result, h_acc_ListOpen, int *n, Uid *out, int max) {
    *n = 0;
    if (max > 0) { out[0] = k_uid; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "用户选择小程序；宿主直接选本地用户", Result, h_acc_ShowSel, Uid *out, const void *settings) {
    (void)settings; *out = k_uid; return RES_OK;
}
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "启动时预选用户；恒为本地用户（返回 bool）", unsigned, h_acc_TryOpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return 1;
}
// 同一件事的 Result 版（HSS/Unity 走这条，饥荒走上面那条 bool 版）。没接管时 SDK 客户端要连 acc:u0 会话，
// 宿主没这个服务 ⇒ 内部等待判错、AbortImpl 自杀（v12 实测：pc=sdk+0x19a368〈OpenPreselectedUser〉→ AbortImpl，调用者 main+0x13c1d64）。
// The Result flavour of the same call (HSS/Unity takes this one, Don't Starve the bool version above). Un-taken-over, the SDK client would
// reach for the acc:u0 session the host does not have, its internal wait reports an error and AbortImpl fires (measured in v12:
// pc=sdk+0x19a368 <OpenPreselectedUser> -> AbortImpl, caller main+0x13c1d64).
HLE_FORCE("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", "预选用户（Result 版）；恒为本地用户", Result, h_acc_OpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "打开用户；句柄内直接存 Uid", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) {
    h->v[0] = uid->v[0]; h->v[1] = uid->v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "从句柄取 Uid", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    if (h->v[2] != HANDLE_MAGIC) die("account::GetUserId 句柄不是宿主发的（%#llx）", h->v[2]);
    out->v[0] = h->v[0]; out->v[1] = h->v[1]; return RES_OK;
}
// nn::account::Nickname = struct { char name[33]; }，UTF-8
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "用户昵称；按 DM_NICK 给值", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    (void)uid;
    const char *e = getenv("DM_NICK");
    memset(nick, 0, 33); strncpy(nick, e ? e : "Player", 32);
    return RES_OK;
}

// ---------- 联网账号：剪 ----------
// ---------- Online accounts: cut ----------
HLE_FORCE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", "网络服务账号，排除范围；剪口：恒不可用", Result, h_acc_EnsureNsa, const UserHandle *h) { (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", "网络服务账号，排除范围；剪口", Result, h_acc_GetNsaId, void *id, const UserHandle *h) { (void)id; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", "网络服务账号令牌，排除范围；剪口", Result, h_acc_LoadTok, uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    (void)buf; (void)sz; (void)h; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", "联网会员提示小程序，排除范围；剪口", Result, h_acc_ShowLic, const UserHandle *h) { (void)h; return RES_FAIL; }

// AsyncContext / NintendoAccountAuthorizationRequestContext：对象内容只由下列接口读写，构造不写内存，完成事件立即触发、结果恒失败
// AsyncContext / NintendoAccountAuthorizationRequestContext: object contents are read and written only by the interfaces below; construction writes no memory, the completion event fires immediately, and the result is always failure
HLE_FORCE("_ZN2nn7account12AsyncContextC1Ev", "异步上下文构造；宿主不建 IPC 对象", void, h_acc_AcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", "网络服务账号令牌，排除范围；剪口", Result, h_acc_EnsureTokAsync, void *ctx, const UserHandle *h) { (void)ctx; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "异步完成事件；立即触发", Result, h_acc_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext9GetResultEv", "异步结果；联网已剪，恒失败", Result, h_acc_AcGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev", "任天堂账号授权，排除范围；构造不写内存", void, h_acc_NaCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev", "任天堂账号授权，排除范围", void, h_acc_NaDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm",
          "任天堂账号授权，排除范围；剪口", Result, h_acc_NaCreate, void *ctx, const UserHandle *h, const void *prm, void *buf, uint64_t sz) {
    (void)ctx; (void)h; (void)prm; (void)buf; (void)sz; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE", "授权完成事件；立即触发", Result, h_acc_NaGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv", "授权结果；恒失败", Result, h_acc_NaGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm", "授权令牌；恒失败", Result, h_acc_NaGetTok, void *self, uint64_t *n, char *buf, uint64_t sz) {
    (void)self; (void)buf; (void)sz; if (n) *n = 0; return RES_FAIL;
}

// ---------- 剩下九个：B28 清点出来还在走 SDK 原实现 ----------
// 每一个落回 SDK 就会去连 acc:u0（宿主没有那个服务）：报文回不对 → 句柄读到栈残值 → 请求发飞 → Result 非 0 →
// nn::diag::detail::AbortImpl 自杀。本地用户与句柄的口径照上面几条：恒一个 k_uid，句柄里存 Uid + HANDLE_MAGIC。
// The remaining nine, inventoried on B28 as still running the SDK's own implementation.
// Each one falling back to the SDK makes it connect to acc:u0 (which the host does not have): a wrong reply -> the
// handle reads stack residue -> the request goes to nonsense -> a non-zero Result -> nn::diag::detail::AbortImpl.
// Local-user and handle conventions follow the entries above: always one k_uid, the handle stores Uid + HANDLE_MAGIC.
HLE_FORCE("_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE", "acc:u0；最后打开的用户 = 宿主本地用户", Result, h_acc_LastOpened, Uid *out) {
    *out = k_uid; return RES_OK;
}
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "acc:u0；本地用户恒存在，全 0 的 Uid 才答不存在", Result,
          h_acc_UserExists, uint8_t *out, const Uid *uid) {
    *out = (uid->v[0] | uid->v[1]) != 0 ? 0 : 1;   // nn 的 bool 是 1 字节，出参按字节写 | nn's bool is one byte, so the out-param is written as a byte
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "acc:u0；句柄是宿主自己存的值，关闭即空操作", Result, h_acc_CloseUser, const UserHandle *h) { (void)h; return RES_OK; }
HLE_FORCE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "网络服务账号，排除范围；答「不可用」比报失败少一条异常分支", Result,
          h_acc_NsaAvail, uint8_t *out, const UserHandle *h) { (void)h; *out = 0; return RES_OK; }

// AsyncContext 的其余成员：对象里没有 IPC 状态，取消/等待都按「已完成、结果失败」（与上面的 GetResult 同一口径）
// The rest of AsyncContext: the object holds no IPC state, so cancel/wait report "done, result failed" (the same stance as GetResult above)
HLE_FORCE("_ZN2nn7account12AsyncContextD1Ev", "异步上下文析构；宿主不建 IPC 对象", void, h_acc_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account12AsyncContext6CancelEv", "异步上下文取消；没有正在跑的任务，直接算成功", Result, h_acc_AcCancel, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext7HasDoneEPb", "异步上下文是否完成；立即完成", Result, h_acc_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "异步上下文移动赋值；对象内无状态，原样返回自己", void *, h_acc_AcMove, void *self, const void *rhs) { (void)rhs; return self; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextaSEOS1_", "任天堂账号授权上下文移动赋值；对象内无状态", void *,
          h_acc_NaMove, void *self, const void *rhs) { (void)rhs; return self; }
