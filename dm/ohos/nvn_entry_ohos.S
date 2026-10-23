    .arch_extension lse
// 未实现 NVN 函数的公共入口：跳板进来时 x16 = 本函数的槽地址（nvn_slot[i]）。
// 调 nvn_unimpl_c(slot, x0, x1, x2) 报红记名，返回 0。
// Common entry for unimplemented NVN functions: when the thunk comes in, x16 = this function's slot address (nvn_slot[i]).
// Calls nvn_unimpl_c(slot, x0, x1, x2) to report a failure and log the name, returns 0.
    .text
    .globl nvn_unimpl_entry
    .p2align 2
nvn_unimpl_entry:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x16
    bl nvn_unimpl_c
    ldp x29, x30, [sp], #16
    ret

// 计时包装入口（DM_NVN_PROF=1 时槽表指向这里）：跳板进来时 x16 = 本函数槽地址。
// 保存 x0-x7 / q0-q7（NVN 函数无栈传参，生成器已核），取 oc_abs_time → 调真实现（nvn_real[i]）→ 再取，
// 差值原子累加到 nvn_prof_ticks[i]（mach 时基单位）；返回值 x0/x1、q0-q3 原样带回。
// 不用 cntvct_el0：本机实测它与系统时基不成固定比例（cntfrq 报 1 GHz，同区间增量比 98.9），不能换算成时间。
// Timing wrapper entry (the slot table points here when DM_NVN_PROF=1): when the thunk comes in, x16 = this function's slot address.
// Save x0-x7 / q0-q7 (NVN functions pass no arguments on the stack, verified by the generator), take oc_abs_time -> call the real implementation (nvn_real[i]) -> take it again,
// atomically add the difference to nvn_prof_ticks[i] (mach timebase units); return values x0/x1, q0-q3 are passed back unchanged.
// cntvct_el0 is not used: measured on this machine it is not in a fixed ratio to the system timebase (cntfrq reports 1 GHz, increment ratio over the same interval 98.9), so it cannot be converted to time.
    .globl nvn_prof_entry
    .p2align 2
nvn_prof_entry:
    sub sp, sp, #240
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    adrp x9, nvn_slot
    add x9, x9, :lo12:nvn_slot
    sub x16, x16, x9
    lsr x16, x16, #4                 // 槽号 i | Slot index i
    str x16, [sp, #80]
    bl oc_abs_time
    str x0, [sp, #88]
    ldr x16, [sp, #80]
    adrp x9, nvn_real
    add x9, x9, :lo12:nvn_real
    ldr x17, [x9, x16, lsl #3]
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    blr x17
    stp x0, x1, [sp, #16]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    bl oc_abs_time
    mov x9, x0
    ldr x10, [sp, #88]
    sub x9, x9, x10
    ldr x16, [sp, #80]
    adrp x10, nvn_prof_ticks
    add x10, x10, :lo12:nvn_prof_ticks
    add x10, x10, x16, lsl #3
    stadd x9, [x10]
    ldp x0, x1, [sp, #16]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp x29, x30, [sp]
    add sp, sp, #240
    ret

// 有序调用环入口（DM_NVN_TRACE=1 时槽表指向这里）：跳板进来时 x16 = 本函数槽地址。
// 记 (槽号, x0..x5, x8, 调用时的 sp) 进 nvn_trace[] 环形缓冲，然后尾跳到真实现 nvn_real[i]。
// x8 是结构体返回的隐藏缓冲区指针：NVN 有一批「按值返回大结构」的取值函数，写坏客体栈的就是这条路，所以必须记。
// 只改 x9..x15 与 x17（x8 要存进环、x16 要原样留给未实现桩，都不动），不开栈帧、不动 sp，
// 所以记录的 sp 就是客体调用方的 sp，崩现场能用它把同一个线程的调用串挑出来。
// Ordered call-ring entry (the slot table points here when DM_NVN_TRACE=1): when the thunk comes in, x16 = this slot address.
// Records (slot, x0..x5, x8, caller sp) into the nvn_trace[] ring, then tail-branches to the real implementation nvn_real[i].
// x8 is the hidden structure-return buffer: a batch of NVN getters return large structs by value and that is the write
// path suspected of clobbering the guest stack, so it must be recorded.
// Touches only x9..x15 and x17 (x8 is saved into the ring, x16 must survive for the unimplemented stubs), allocates no
// frame and never moves sp, so the recorded sp is the guest caller's own sp -- the crash dump uses it to pick out one
// thread's call chain.
    .globl nvn_trace_entry
    .p2align 2
nvn_trace_entry:
    adrp x9, nvn_slot
    add x9, x9, :lo12:nvn_slot
    sub x12, x16, x9
    lsr x12, x12, #4                 // 槽号 i；x16 原样留着，未实现桩还要用它当槽地址 | Slot index i; x16 stays intact, the unimplemented stub needs it as the slot address
    adrp x10, nvn_trace_n
    add x10, x10, :lo12:nvn_trace_n
    mov x11, #1
    ldadd x11, x13, [x10]            // x13 = 自增前的序号 | sequence before increment
    and x13, x13, #255
    lsl x13, x13, #7                 // 每条 128 字节 | 128 bytes per entry
    adrp x14, nvn_trace
    add x14, x14, :lo12:nvn_trace
    add x13, x13, x14
    str x12, [x13]
    str x0, [x13, #8]
    str x1, [x13, #16]
    str x2, [x13, #24]
    str x3, [x13, #32]
    str x4, [x13, #40]
    str x5, [x13, #48]
    str x8, [x13, #56]
    mov x15, sp
    str x15, [x13, #64]
    adrp x14, nvn_real
    add x14, x14, :lo12:nvn_real
    ldr x17, [x14, x12, lsl #3]
    br x17
