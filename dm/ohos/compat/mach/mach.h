// OHOS 兼容替身：mach 内存查询 + 线程接口 → /proc 实现 / 空实现（oc_* 在 ohos_compat.c）
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
typedef int kern_return_t;
#define KERN_SUCCESS 0
typedef int task_t;
typedef uint64_t mach_port_t;
typedef vm_address_t task_info_t;
typedef int mach_msg_type_number_t;
#define TASK_VM_INFO 22
typedef struct {
    uint64_t virtual_size, resident_size, resident_size_max;
    uint64_t phys_footprint;
    uint64_t _pad[32];
} task_vm_info_data_t;
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
// 线程状态（iOS 现场导出用；OHOS 上 mport 恒 0，导出函数自行短路）
typedef struct {
    uint64_t __x[29];
    uint64_t __fp, __lr, __sp, __pc;
    uint32_t __cpsr, __pad;
} arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
typedef uint32_t *thread_state_t;
typedef int boolean_t;
typedef int integer_t;
typedef struct { integer_t seconds; integer_t microseconds; } time_value_t;
typedef integer_t *thread_info_t;
typedef integer_t *thread_policy_t;
#define THREAD_BASIC_INFO 3
typedef struct {
    time_value_t user_time, system_time;
    integer_t cpu_usage, policy, run_state, flags, suspend_count, sleep_time;
    char _pad[64];
} thread_basic_info_data_t;
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
#define THREAD_EXTENDED_INFO 5
typedef struct {
    integer_t pth_curpri, pth_priority, pth_policy, pth_maxpriority;    // 代码只读这三项 | the code only reads these three
    char _pad[512];
} thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))
#define THREAD_TIME_CONSTRAINT_POLICY 2
typedef struct { integer_t period, computation, constraint, preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
typedef uint32_t thread_act_t;
typedef thread_act_t *thread_act_array_t;
#ifdef __cplusplus
extern "C" {
#endif
task_t mach_task_self(void);
kern_return_t task_info(task_t task, int flavor, task_info_t info, mach_msg_type_number_t *count);
kern_return_t vm_read_overwrite(task_t task, vm_address_t addr, vm_size_t size, vm_address_t dst, vm_size_t *out_size);
mach_port_t mach_thread_self(void);
mach_port_t pthread_mach_thread_np(pthread_t t);
kern_return_t thread_suspend(mach_port_t t);
kern_return_t thread_resume(mach_port_t t);
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t state, mach_msg_type_number_t *count);
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t info, mach_msg_type_number_t *count);
kern_return_t thread_policy_get(mach_port_t t, int flavor, thread_policy_t policy, mach_msg_type_number_t *count, boolean_t *is_default);
kern_return_t thread_policy_set(mach_port_t t, int flavor, thread_policy_t policy, mach_msg_type_number_t count);
kern_return_t task_threads(task_t task, thread_act_array_t *acts, mach_msg_type_number_t *count);
kern_return_t mach_port_deallocate(task_t task, mach_port_t name);
kern_return_t vm_deallocate(task_t task, vm_address_t addr, vm_size_t size);
kern_return_t mach_wait_until(uint64_t deadline);
int thread_selfcounts(int type, void *counts, size_t n);   // Apple 私有：指令/周期计数；OHOS 无对应物，回 -1（读数记 0）
#ifdef __cplusplus
}
#endif
static inline uint64_t __darwin_arm_thread_state64_get_pc(arm_thread_state64_t st) { return st.__pc; }
static inline uint64_t __darwin_arm_thread_state64_get_fp(arm_thread_state64_t st) { return st.__fp; }
static inline uint64_t __darwin_arm_thread_state64_get_lr(arm_thread_state64_t st) { return st.__lr; }
static inline uint64_t __darwin_arm_thread_state64_get_sp(arm_thread_state64_t st) { return st.__sp; }
