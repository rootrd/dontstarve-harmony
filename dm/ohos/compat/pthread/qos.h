// OHOS 兼容替身：QoS 类无对应物，全部空实现（取值只为诊断记录）
#pragma once
#include <pthread.h>
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0x00 } qos_class_t;
typedef int pthread_priority_t;
static inline int pthread_set_qos_class_self_np(qos_class_t c, int rel) { (void)c; (void)rel; return 0; }
static inline int pthread_attr_set_qos_class_np(void *a, qos_class_t c, int rel) { (void)a; (void)c; (void)rel; return 0; }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; *q = QOS_CLASS_UNSPECIFIED; *rel = 0; return 0; }
