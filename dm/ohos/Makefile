# OHOS (aarch64-linux-ohos) 端口构建 —— 在 Linux 构建机上跑：
#   make -f dm/ohos/Makefile sweep   全量编译扫描（收集错误）
#   make -f dm/ohos/Makefile obj     编到 build/
HERE := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
HOST := $(HERE)/../host
GFX  := $(HERE)/../gfx
COMPAT := $(HERE)/compat
PLACE  := $(HERE)/placeholder
BUILD  := $(HERE)/build
OHOS_SYSROOT ?= $(HOME)/ohos/sysroot
CROSS ?= clang --target=aarch64-linux-ohos --sysroot=$(OHOS_SYSROOT)
CFLAGS := -O1 -g -Wall -Wno-unused-function -D_GNU_SOURCE \
  -I$(HOST) -I$(GFX)/nvn -I$(GFX)/nvn/include -I$(COMPAT) -I$(PLACE) \
  -I$(HERE)/../../third_party/astcenc/include -ffunction-sections -fdata-sections \
  -include $(COMPAT)/ohos_force.h
# B 路（设备自转 BC3→ASTC）的静态库：构建机交叉编的 astcenc 5.7.0（NEON）+ 静态 libc++。
# .so 里带 C++ 运行时的唯一原因是 astcenc 是 C++；-gc-sections 把没用到的部分剪掉，别让 11 MB 的 libc++ 全进包。
# Round B's static archives: the cross-built astcenc (NEON) plus a static libc++. --gc-sections keeps the .so from carrying all of it.
ASTC_DIR := $(HERE)/../../third_party/astcenc/ohos
ASTC_LIBS := $(ASTC_DIR)/libastcenc-neon-static.a $(ASTC_DIR)/libc++_static.a $(ASTC_DIR)/libc++abi.a

# Apple 专属不编（front_mac.m app_mac.m memstat.m touch_mac.c）；重放层 .m 待以 nvn_vulkan.c 替代
HOST_SRC := $(HOST)/main.c $(HOST)/loader.c $(HOST)/hle.c $(HOST)/crash.c $(HOST)/diag.c \
  $(HOST)/thread.c $(HOST)/romfs_validate.c \
  $(wildcard $(HOST)/dm_*.c) $(wildcard $(HOST)/hle_*.c)
GFX_SRC := $(wildcard $(GFX)/*.c) $(wildcard $(GFX)/nvn/*.c) $(wildcard $(GFX)/tex/*.c)
TD := $(HERE)/../third_party/libdeflate/lib
LIBDEFLATE := $(TD)/deflate_decompress.c $(TD)/zlib_decompress.c $(TD)/adler32.c $(TD)/utils.c $(TD)/arm/cpu_features.c
COMPAT_SRC := $(COMPAT)/ohos_compat.c $(COMPAT)/md5.c $(COMPAT)/sha1.c $(COMPAT)/sha256.c
ALL_SRC := $(HOST_SRC) $(GFX_SRC) $(LIBDEFLATE) $(COMPAT_SRC)

.PHONY: sweep obj clean
sweep:
	@mkdir -p $(BUILD); rm -f $(BUILD)/errors.txt
	@ok=0; fail=0; for f in $(ALL_SRC); do o=$(BUILD)/$$(basename $$f .c).o; \
	  if $(CROSS) $(CFLAGS) -c -o $$o $$f 2>>$(BUILD)/errors.txt; then ok=$$((ok+1)); else fail=$$((fail+1)); echo "FAIL $$f"; fi; done; \
	  echo "== sweep: ok=$$ok fail=$$fail =="; echo "-- unique errors --"; \
	  grep "error:" $(BUILD)/errors.txt 2>/dev/null | sed 's/^.*[\/]//' | sort | uniq -c | sort -rn | head -40; true
obj:
	@mkdir -p $(BUILD); for f in $(ALL_SRC); do $(CROSS) $(CFLAGS) -c -o $(BUILD)/$$(basename $$f .c).o $$f || exit 1; done; echo "obj ok"
clean:
	rm -rf $(BUILD)

# ---- 链接目标：先出可执行（钓未定义符号用），后出 libdmhost.so（NAPI 模块）----
OHOS_ALL_C := $(ALL_SRC) $(HERE)/front_ohos.c $(HERE)/audio_ohos.c
link:
	@mkdir -p $(BUILD)
	@for f in $(OHOS_ALL_C); do $(CROSS) $(CFLAGS) -c -o $(BUILD)/$$(basename $$f .c).o $$f || exit 1; done
	$(CROSS) -c -o $(BUILD)/entry_ohos.o $(HERE)/entry_ohos.S
	$(CROSS) -c -o $(BUILD)/nvn_thunks_ohos.o $(HERE)/nvn_thunks_ohos.S
	$(CROSS) -c -o $(BUILD)/nvn_entry_ohos.o $(HERE)/nvn_entry_ohos.S
	$(CROSS) -fuse-ld=lld -Wl,--error-limit=0 -o $(BUILD)/dmhost.ohos $(BUILD)/*.o
	@echo "== link ok =="; file $(BUILD)/dmhost.ohos


# ---- 共享库（NAPI 模块 libdmhost.so）：全量 -fPIC + app_ohos.c 入口，-Bsymbolic 让汇编 adrp/add 绑本地符号 ----
SOLIB_C := $(ALL_SRC) $(HERE)/front_ohos.c $(HERE)/audio_ohos.c $(HERE)/pad_ohos.c $(HERE)/app_ohos.c $(HERE)/xcomp_ohos.c $(HERE)/vk_smoke.c
solib:
	@mkdir -p $(BUILD)/pic
	@for f in $(SOLIB_C); do $(CROSS) $(CFLAGS) -fPIC -c -o $(BUILD)/pic/$$(basename $$f .c).o $$f || exit 1; done
	$(CROSS) -fPIC -c -o $(BUILD)/pic/entry_ohos.o $(HERE)/entry_ohos.S
	$(CROSS) -fPIC -c -o $(BUILD)/pic/nvn_thunks_ohos.o $(HERE)/nvn_thunks_ohos.S
	$(CROSS) -fPIC -c -o $(BUILD)/pic/nvn_entry_ohos.o $(HERE)/nvn_entry_ohos.S
	$(CROSS) -shared -fPIC -fuse-ld=lld -Wl,-Bsymbolic -Wl,--error-limit=0 -o $(BUILD)/libdmhost.so $(BUILD)/pic/*.o $(ASTC_LIBS) -lm -lpthread -lace_napi.z -lhilog_ndk.z -lvulkan
	@file $(BUILD)/libdmhost.so | cut -c1-140
