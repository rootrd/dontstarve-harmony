# Dependencies

Everything the build needs, what each item is for, and where to get it. This is the HarmonyOS branch: the toolchain is an AArch64/OpenHarmony cross-compiler plus the DevEco packaging tools — no Xcode, no Apple signing, nothing that needs a Mac.

中文：[DEPENDENCIES.zh-CN.md](DEPENDENCIES.zh-CN.md)

## What you bring yourself

| What | Used for | Notes |
|---|---|---|
| Your own game files | The guest program and all game data | This repository contains none of them. You need the Switch build's three program modules (`main`, `sdk`, `subsdk0`) and its romfs, taken from a copy you own. See [UNPACKING.md](UNPACKING.md) |
| A HarmonyOS phone with developer mode | The only real test platform | Maleoon 920 GPU. Nothing here is emulatable — the whole point of the device round is the vendor driver's actual behaviour |
| Your own signing material (certificate + profile) | Making an installable HAP | Generate it in DevEco Studio against your Huawei developer account. **Never commit it** — `.gitignore` blocks `*.p12`, `*.p7b`, `*.cer` |

## Build machine (Linux, macOS or WSL)

| Tool | Version we build with | Used for | Get it |
|---|---|---|---|
| clang | any release with the `aarch64-unknown-linux-ohos` target | compiling and linking `libdmhost.so` | <https://releases.llvm.org> |
| OpenHarmony sysroot | the native sysroot that ships with the SDK below | headers and stub libraries for `-lace_napi.z -lhilog_ndk.z -lvulkan` | DevEco Studio's SDK manager |
| GNU make | — | `dm/ohos/Makefile` | system package |
| unzip, ssh, scp | — | reading artefacts back out of a HAP, driving a remote build machine | system package |

The host is plain C. No OHOS C++ runtime is linked, and no NDK C++ headers are used — that is deliberate, because the guest supplies its own C++ runtime.

## Packaging and install (Windows, macOS or Linux)

| Tool | Version | Used for |
|---|---|---|
| DevEco Studio, or the standalone `hvigor` + `hdc` command-line toolchains | ships SDK `6.1.1(24)`; `hap/build-profile.json5` pins `compatibleSdkVersion: 6.1.1(24)`, `targetSdkVersion: 26.0.0` | assembling, signing and installing the two-module HAP |
| Node.js | whatever DevEco ships | `hvigorw` is a Node script |
| A JDK on `PATH` | 17 or newer | `hvigorw` shells out to Java; without this the headless build fails in a way that looks like a project error |
| `hdc` | with the SDK above | file transfer, install, `aa start`, `uitest uiInput`, `snapshot_display`, `hidumper`, `top -H` |

## Offline analysis (this is where most of the debugging happens)

| Tool | Used for | Get it |
|---|---|---|
| Python 3 + `capstone` | disassembling guest code around a crash address; the tools in `dm/tools/` | `pip install capstone` |
| Python 3 + `numpy` | the texture/format offline converters | `pip install numpy` |
| hactool | unpacking your own `.nsp`/`.xci` into ExeFS + romfs | <https://github.com/SciresM/hactool>, built with `make` |
| glslang (`glslangValidator -V --target-env vulkan1.0`) | compiling our own GLSL into the SPIR-V embedded in `dm/gfx/nvn/nvn_sh_tri.h` | LunarG SDK or <https://github.com/KhronosGroup/glslang> |
| PowerShell + `System.Drawing` | image work on a Windows development host that has no usable Python/PIL. `Add-Type` needs `-ReferencedAssemblies System.Drawing`, and **C# here-string bodies must stay ASCII-only** — it reads the script as ANSI, so non-ASCII literals fail with what looks like a syntax error | built into Windows |

## Third-party code in this repository

| Component | Licence | Where | Used for |
|---|---|---|---|
| NVN API declarations | MIT (author credited in the file header) | `dm/gfx/nvn/include/nvn.h` | the entry points the replay layer implements |
| Ryujinx shader decoder (subset) | MIT | `dm/third_party/ryujinx/` | decoding guest NVN control blocks so shaders can be re-emitted for Vulkan |
| bcdec | MIT (or Apache-2.0, at your choice) | `dm/third_party/bcdec/` | BC1–BC7 texture decompression |
| flip, stb_image, tinyexr | BSD-3-Clause / MIT / public domain, per file | `dm/third_party/flip/` | EXR and GPU-texture formats |
| libdeflate | zlib | `dm/third_party/libdeflate/` | the only decompressor linked into the host |

Each keeps its own licence file in place; the same table, with provenance, is in [NOTICE](../NOTICE). If you redistribute this tree, keep them.
