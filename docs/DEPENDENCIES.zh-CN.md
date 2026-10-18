# 依赖与外部工具

构建要用的每一样东西、各自的用途、去哪儿拿。本仓库是鸿蒙分支：工具链是 AArch64/OpenHarmony 交叉编译器 + DevEco 打包工具链，**不需要 Mac，也不需要 Xcode 和苹果开发者账号**。

English: [DEPENDENCIES.md](DEPENDENCIES.md)

## 需要你自己准备的

| 东西 | 用来做什么 | 说明 |
|---|---|---|
| 你自己的游戏文件 | 客体程序与全部游戏数据 | 本仓库一个字节都不带。要的是 Switch 版三个程序模块（`main`、`sdk`、`subsdk0`）和它的 romfs，从**你自己拥有**的副本里取。见 [UNPACKING.md](UNPACKING.md) |
| 一台开了开发者模式的鸿蒙手机 | 唯一真实的测试平台 | Maleoon 920 GPU。这里没有任何东西能在模拟器里验——整件事的意义就在于厂商驱动的实际行为 |
| 你自己的签名材料（证书 + Profile） | 打出可安装的 HAP | 在 DevEco Studio 里用你的华为开发者账号生成。**绝不提交**——`.gitignore` 已挡 `*.p12`、`*.p7b`、`*.cer` |

## 构建机（Linux / macOS / WSL 都行）

| 工具 | 我们用的版本 | 用来做什么 | 哪儿拿 |
|---|---|---|---|
| clang | 任何带 `aarch64-unknown-linux-ohos` 目标的发行版 | 编译并链接 `libdmhost.so` | <https://releases.llvm.org> |
| OpenHarmony sysroot | 下面那套 SDK 自带的 native sysroot | `-lace_napi.z -lhilog_ndk.z -lvulkan` 的头与存根库 | DevEco Studio 的 SDK Manager |
| GNU make | — | 跑 `dm/ohos/Makefile` | 系统包 |
| unzip、ssh、scp | — | 把产物从 HAP 里读回来核对、驱动远程构建机 | 系统包 |

宿主是纯 C。不链接鸿蒙的 C++ 运行时，也不用 NDK 的 C++ 头——这是故意的，客体的 C++ 运行时由客体自己带。

## 打包与装机（Windows / macOS / Linux）

| 工具 | 版本 | 用来做什么 |
|---|---|---|
| DevEco Studio，或独立的 `hvigor` + `hdc` 命令行工具链 | 随附 SDK `6.1.1(24)`；`hap/build-profile.json5` 里钉了 `compatibleSdkVersion: 6.1.1(24)`、`targetSdkVersion: 26.0.0` | 组装、签名、安装这个双模块 HAP |
| Node.js | DevEco 自带的那个 | `hvigorw` 本身是 Node 脚本 |
| `PATH` 上一个 JDK | 17 及以上 | `hvigorw` 要调 Java；没有它，无头构建会失败成一副"工程坏了"的样子 |
| `hdc` | 随上面那套 SDK | 传文件、装机、`aa start`、`uitest uiInput`、`snapshot_display`、`hidumper`、`top -H` |

## 离线分析（大部分调试是在这里完成的，不在手机上）

| 工具 | 用来做什么 | 哪儿拿 |
|---|---|---|
| Python 3 + `capstone` | 反汇编崩溃地址附近的客体代码；`dm/tools/` 里那批脚本 | `pip install capstone` |
| Python 3 + `numpy` | 贴图/格式转换的离线工具 | `pip install numpy` |
| hactool | 把**你自己的** `.nsp`/`.xci` 拆成 ExeFS + romfs | <https://github.com/SciresM/hactool>，`make` 出来 |
| glslang（`glslangValidator -V --target-env vulkan1.0`） | 把我们自己的 GLSL 编成 `dm/gfx/nvn/nvn_sh_tri.h` 里内嵌的 SPIR-V | LunarG SDK 或 <https://github.com/KhronosGroup/glslang> |
| PowerShell + `System.Drawing` | 开发机是 Windows、又没有可用 Python/PIL 时做图像活。`Add-Type` 要加 `-ReferencedAssemblies System.Drawing`，并且 **C# 内嵌代码体必须保持纯 ASCII**——它是按 ANSI 读脚本的，非 ASCII 字面量会以"语法错误"的面目失败 | Windows 自带 |

## 随仓库的第三方代码

| 组件 | 许可证 | 位置 | 用来做什么 |
|---|---|---|---|
| NVN API 声明 | MIT（作者见文件头） | `dm/gfx/nvn/include/nvn.h` | 重放层要实现的那批入口 |
| Ryujinx 着色器解码器（子集） | MIT | `dm/third_party/ryujinx/` | 解出客体 NVN 控制段，好重新生成给 Vulkan 的着色器 |
| bcdec | MIT / Apache-2.0（任选） | `dm/third_party/bcdec/` | BC1–BC7 贴图解压 |
| flip、stb_image、tinyexr | BSD-3-Clause / MIT / 公有领域（逐文件） | `dm/third_party/flip/` | EXR 与 GPU 贴图格式 |
| libdeflate | zlib | `dm/third_party/libdeflate/` | 唯一链进宿主的解压器 |

每一份都保留了自己的许可证文件在原位；同一张表连同来源声明在 [NOTICE](../NOTICE) 里。要再分发这棵树，请一并带上它们。
