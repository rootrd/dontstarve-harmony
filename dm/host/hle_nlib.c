// nlib 堆（nn::mem::nlib，Unity/IL2CPP 在 Switch 上的分配器）的错误上报口。
// HSS 实测（v9）：客体读完 Data/Managed/Metadata/global-metadata.dat 之后，SDK 的
// nlibsdk::heap::CentralHeap::Free 走到 `bl NotNMallocPtrError`（sdk+0x125060），那个函数里就是
// nnmem_nlib_debug_break（sdk+0x119190，`e7ffdefe`）→ SIGILL，进程没了。
// 语义是「要释放的指针不属于这块中央堆」：真机上这是致命断言。宿主这边先记账放行——
// Free 拿到调用者本来就回的错误码 0xE（不释放、不破坏堆），指针不匹配时只是漏一小块内存。
// 这是「继续往前跑 + 把现场数出来」的接法，不是根因修复：根因要么是客体把两个分配器的指针用串了，
// 要么是宿主给客体的堆/别名区边界（svc GetInfo 21/22 那套）和 nlib 自己的区域重叠。计数和指针值就是判据。
// The nlib heap (nn::mem::nlib, Unity/IL2CPP's allocator on Switch) error reporter.
// Measured on HSS (v9): after the guest reads Data/Managed/Metadata/global-metadata.dat, the SDK's
// nlibsdk::heap::CentralHeap::Free reaches `bl NotNMallocPtrError` (sdk+0x125060), and that function *is*
// nnmem_nlib_debug_break (sdk+0x119190, `e7ffdefe`) -> SIGILL, process gone.
// The meaning is "the pointer being freed is not owned by this central heap"; on real hardware that is a fatal assert.
// Here the host books it and lets the caller continue: Free returns the error code 0xE the caller already expects
// (nothing is freed, the heap is not corrupted), so a mismatched pointer just leaks a small block.
// This is "keep running and count the scene", not the root fix. The root cause is either the title mixing two
// allocators' pointers, or the host's heap/alias region bounds (the svc GetInfo 21/22 pair) overlapping the area the
// nlib heap claims. The counts and pointer values are the evidence.
#include "hle.h"
#include <stdatomic.h>

HLE_FORCE("_ZN2nn7nlibsdk4heap18NotNMallocPtrErrorEPKv", "nlib 堆：指针不属于中央堆；宿主记账放行（真机在此断言自杀）", void, h_nlib_NotMallocPtr, const void *p) {
    static _Atomic unsigned n;
    unsigned k = atomic_fetch_add(&n, 1);
    if (k < 40) LOG("nlib 堆 #%u：%p 不属于中央堆（调用者 %p）→ 不释放，继续", k + 1, p, __builtin_return_address(0));
}
