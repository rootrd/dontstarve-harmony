# 饥荒联机版（Switch）跑在鸿蒙真机上 · HLE 原生移植

**Don't Starve Together** 的 Nintendo Switch 版本，在华为鸿蒙手机上**原生执行**：不是模拟器套壳、不是重制、不是云游戏。客体的 ARM64 机器码直接跑在我们的 HLE（High-Level Emulation）宿主里，它调用的每一件系统能力——线程与信号量、文件系统、socket、手柄、音频、以及 **NVN 图形 API**——都由鸿蒙的原生实现顶上，NVN 被逐调用**重放进 Vulkan**，画在 Maleoon 920 上。

**当前状态（2026-10-18）**：**已经能完整进游戏**——EULA → 主菜单 → 创建世界 → 世界生成 → 选角色 → 走进世界、第 1 天 HUD 正常。画面还差最后一件事：**地面贴图是纯绿的**（角色、草丛、光照、HUD、文字全对）。

<p align="center">
  <img src="docs/screens/main_menu.jpg" width="49%" alt="真主菜单：Wilson、资料/选项/购物/退出、右上「离线」">
  <img src="docs/screens/in_world.jpg" width="49%" alt="进世界后：第 1 天 HUD、物品栏、常青树交互提示都对，地面是纯绿">
</p>
<p align="center"><sub>左：主菜单已完整渲染。右：已进世界（第 1 天、物品栏、"常青树"交互提示都在），地面纯绿 = 目前唯一的功能卡点。</sub></p>

平台：Huawei 真机（HarmonyOS NEXT，Maleoon 920，2848×1276）· 客体版本：Switch 版饥荒联机版 · 许可：MIT · **游戏文件自带（BYOG），本仓库不含任何游戏内容**

English: [README.md](README.md) · 详细进度：[docs/STATUS.md](docs/STATUS.md) · 卡点：[docs/BLOCKERS.md](docs/BLOCKERS.md) · 解包：[docs/UNPACKING.md](docs/UNPACKING.md)

> `docs/` 下的技术文档目前只有英文版（STATUS / BLOCKERS / UNPACKING / BUILD-OHOS / DM-SWITCHES / CONTENTS）；`DEPENDENCIES` 有中英双语。

---

## 为什么是这条路

Switch 上的游戏要在别的平台跑，通常只有两条路：写一个完整的 Horizon OS 模拟器（要 CPU JIT、要全套服务、要着色器编译器），或者——如果这款游戏**只用了很少的平台服务**——把它当成一个"目标 ABI 换掉的动态库"，用 HLE 直接把系统调用和 API 一一实现出来。饥荒属于后者：它的图形走 NVN（可以机械地映射到 Vulkan），它的平台需求集中在少量 `nn::*` 服务上，而它的数据（romfs）可以原样读。

同一套宿主此前已经跑通了《暗黑破坏神 II：重制版》的 Switch 版（见 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)，同一作者，MIT）。本仓库是它的**鸿蒙分支**：宿主层复用，新写的是 HarmonyOS 端口（NAPI/XComponent/OHAudio/触控）与 **NVN→Vulkan 重放层**（`dm/gfx/nvn/nvn_vulkan.c`）。

## 一图看懂数据怎么流

```
hdc aa start --ps DM_xxx  ──►  ArkTS 入口（EntryAbility）把参数交给 NAPI
                                 ─► libdmhost.so：装载 main / sdk / subsdk0（ELF + 指令级补丁表）
                                     ├─ SVC 层     hle_svc.c      含 pre-4.0 语义量族 0x1A–0x1D
                                     ├─ 线程层     hle_thread.c   条件变量 / 伪唤醒兜底
                                     ├─ 文件系统   hle_fs.c       romfs 挂载 + 三级路径兜底 + 全量 basename 索引
                                     ├─ 网络       hle_net.c      真 BSD socket（游戏自己的本机 shard 要它）
                                     ├─ 输入       hle_hid.c      屏幕手柄 / 触控 → Npad
                                     ├─ 音频       hle_audio.c + ohos/audio_ohos.c（OHAudio）
                                     └─ 图形       gfx/nvn/nvn_vulkan.c   NVN 调用逐条重放进 Vulkan
                                          └─ 内存池 → 打包队列 → BC/ASTC 软解 → staging → vkUploadTexture
```

一个有用的性质：**客体模块内的文件偏移与内存偏移 1:1**。所以任何崩溃地址 `pc=<模块>+<偏移>` 都能离线反查——直接查 ELF 的 `.dynamic`/`.dynsym`/`.rela.plt`，不需要在设备上装调试器。这条性质让"真机跑一轮 → 离线定位到具体符号"变成一个 5 分钟的动作。

## 已经拿下的（挑有代表性的）

完整的带判据清单在 [docs/BLOCKERS.md](docs/BLOCKERS.md) 与 [docs/STATUS.md](docs/STATUS.md)；这里只说几条能省别人几小时的：

- **启动链全通**：`nnosInitialize` → SDK 模块初始化 → 三模块全局构造（301+112+45 项）→ `nnMain` → romfs 挂载 → 12+ 个客体线程 → 音频打开。
- **一批服务缺口**：eShop、Joy-Con 分离态一族、默认音频输出重载、`nn::oe::SetCpuBoostMode`、`nn::pctl::IsFreeCommunicationAvailable`。共同机制是客体的 `sm::Initialize()` 连不上服务 → SDK 读到无效响应 → `diag::AbortImpl` 自杀。**剪口只加在本项目主程序不引用的符号上**，零基线风险。
- **NVN→Vulkan 重放层**：补齐 18 个客体要但宿主没有的入口；视口 Y 翻（含裁剪盒）修上下镜像；**UI 平涂的真因是 NVN→Vk 混合因子表错位**；着色器库从 96 键推到 222 键，`无库` 占比 41% → 10%。
- **贴图上传链**：巨图另开暂存（不再挤 64 MB arena）、脏页重传、缺页钩子在 OHOS 上从零接起来（`OC_ES_ESR` 在非 Apple 平台恒 0，所以"写"这件事必须自己判定）。
- **socket 层从"一律返回 ENETDOWN"改成真 BSD**：饥荒的单机模式会在自己进程里起两个 shard（Server Master / Secondary），用 RakNet 连 `127.0.0.1`。没有真 socket 就永远卡在"启动服务器"。
- **素材路径解析**：三层兜底 + 惰性全量 basename 索引，**离线穷举 21,310 个文件全量重筛**，真机判据是"未解析清单文件根本没被创建过"。
- **取证通道**：像素走文件不走日志（hilog 会静默吞长行和突发行）、崩溃现场落文件、线程转储、采样行带上"最近一次 SVC 号"（一眼分辨"在等"和"在 SVC 里空转"）。

## 卡在哪

**地面贴图纯绿**（唯一功能卡点）。证据：地形图集（2048×2048 与 1024×512 那几张）被绑定 **266,425 次**，但一次都没进过上传队列；绑定现场是 `u0 d0 b0 t0 r0`（没传过、没画过、没排过队）。已经否掉两条假说（mip 钳到 0 级、素材文件缺失）。分流办法是两条探针：队列到底有没有排进（`pkg_queue`），以及着色器到底有没有看那个槽（`set_fill` 的全空分支）。细节、判据、下一步都在 **[docs/BLOCKERS.md](docs/BLOCKERS.md)**。

另外四条已知的账：丢唤醒的**根因**还没修（现在靠伪唤醒兜底）、打包暂存与顶点流抢同一块 arena、性能（约 30 fps、p99 250 ms、世界生成 2m20s 而原版 20–40s）、真输入框与触控 finger 上报。

## 你要自己跑一遍

1. **[docs/UNPACKING.md](docs/UNPACKING.md)** —— 用**你自己合法获得**的游戏介质与**你自己设备导出的**密钥，取出三个模块与 romfs，跑指令级补丁，打成两层 romfs 目录。本仓库不带这些产物。
2. **[docs/BUILD-OHOS.md](docs/BUILD-OHOS.md)** —— 交叉编出 `libdmhost.so`，塞进 HAP 工程，签名装机。
3. **[docs/DM-SWITCHES.md](docs/DM-SWITCHES.md)** —— 启动参数（诊断开关全在这里，不用读代码）。
4. **[docs/CONTENTS.md](docs/CONTENTS.md)** —— 这个仓库里**有什么、没有什么**，以及每一条为什么必须在。

## 免责与许可

本仓库只包含我们自己写的与第三方开源的**代码和文档**：**不含**游戏程序、美术、音频、文本、数据表、着色器二进制，也不含任何密钥。所有游戏内容都要由使用者从**自己拥有的**游戏副本里生成。饥荒联机版是 Klei Entertainment 的作品，Nintendo Switch 是任天堂的商标；本项目与两者均无关联，也不为它们背书。请勿用它做任何侵犯版权的事。

代码以 MIT 许可发布（见 [LICENSE](LICENSE)）；来源与第三方声明见 [NOTICE](NOTICE)。
