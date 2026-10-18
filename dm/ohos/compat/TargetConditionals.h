// OHOS 兼容替身：Apple TargetConditionals 全部置 0（走「非 Apple」分支），另加 TARGET_OS_OHOS=1
// OHOS compat shim: all Apple TARGET_OS_* are 0 (take the non-Apple branches); TARGET_OS_OHOS=1
#pragma once
#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_OHOS 1
