# DST 1.30.0 / OHOS 渲染与构建调查

2026-10-23：本轮完成源码修复、Windows 自动签名、完整资源安装及新版 entry 覆盖安装。
Maleoon 910 / Vulkan 1.2 上，启动画面、中文世界设置界面的文字和边框已正常对齐，
用户复测反馈“好像正常了”。此前的拉丝三角形、竖条和文字错位在当前已观察界面中消失。
这是当前构建的验证结果；历史 AN 轮和旧 shader 库的测量见 `BLOCKERS.md`。
尚未逐场景覆盖全部世界内容，也没有完成独立 GPU 读回探针验证。

## 文字跳动、几何错位：关键修复

涉及 `dm/gfx/nvn/nvn_vulkan.c` 的 `arena_put`、`arena_rewritten`、`vtx_stream`、
`vtx_const_binding`、`pipe_get` 和 `do_draw`。

| 问题 | 原因与修复 | 证据 |
|---|---|---|
| arena 分配偏移不对齐 | 旧公式先 `& ~15` 再 `+ 15`，对齐长度反而多出 15 字节，下一次分配落在奇数偏移。改成 `(reserve + 15) & ~15`，并用减法检查剩余容量，拒绝溢出 | UBSan 在旧行为下捕获到未对齐的 `uint16_t` 索引访问；设备明确要求 `uboAlign=16`。索引、顶点和 UBO 共用该 arena，错误偏移会影响多种绘制数据 |
| 同地址动态缓冲可能复用旧副本 | dirty tracking 注册成功不代表页持续受写保护。默认关闭重新武装时，CPU 再次改写同地址不会继续推进脏页代号。arena 哈希缓存及独立顶点流缓存现在都比较完整缓存范围，变化时分配新副本，保留早先命令引用的内容 | 回归模拟“已注册、脏页代号永不变化”，改写 4 KiB 之后的顶点和短索引表，验证新旧副本正确且互不覆盖 |
| 顶点绑定越过设备上限 | 旧代码固定使用 binding 16 做常数零流，且一次绑定 17 个缓冲；设备上限为 16。改为从 0–15 中选择未被真实顶点流使用的槽，仅在需要回退属性时加入管线描述；命令绑定数量限制为 16 | 真机 `maxVertexInputBindings=16`；生产规划函数遍历全部 65,536 种流占用组合通过；当前采样管线创建失败为 0 |

这三项同时进入最终真机版本，未分别做逐项回退的同场景实验，因此不把某一个修复
单独宣称为唯一根因。可确定的是旧代码违反了缓冲区对齐和设备绑定上限，且缓存的
完整性检查存在盲区。压缩纹理解码会影响贴图内容；本轮几何撕裂和文字错位还必须检查
顶点、索引及常量缓冲，不能只凭画面把问题归到 BC/ASTC。

运行仍保留正常去重：`DM_ARENA_NODUP` 默认关闭，`DM_ARENA_ARM` 默认关闭。
前者仅用于对照实验，后者此前有冻结记录；本次不依靠强制关闭去重或重新武装脏页来修复。
当前采样累计 **273,349 次绘制**：无库 0、无管线 0、arena 满 0、描述符耗尽 0、管线创建失败 0。
这些计数只证明采样范围内的执行结果；完整缓存比较的回归才覆盖“4 KiB 之后改写”的情况，
历史 `陈旧0` 的前缀比较计数无法排除这种错误。

本地证据保存在被忽略的 `build/device-evidence/`：`baseline-unique.png` 为旧版拉丝画面，
`arena-fixed-01.png` 为新版启动画面，`arena-fixed-04.png` 为中文世界设置及启动服务器界面，
`arena-fixed-renderer.log` 为新版默认去重开启的日志。截图和运行日志不随代码提交。

## 参考与定位

参考 VintagePomeloPro 的 `docs/graphics/GFX_WORKAROUND_LEDGER.md` 和
`docs/DXVK26_HEAVEN_MALEOON_FIX.md`，应用于本项目的原则是：

- BC 降级保留 sRGB、SNORM 与分量数量：BC4 → R8、BC5 → RG8、BC6H → RGBA16F。
- 查询具体格式的能力，并启用 Vulkan device feature；查询到格式并不代表已经启用压缩采样。
- 可采样不等于可作为颜色附件；SNORM、BC、ASTC 不应统一加 COLOR_ATTACHMENT usage。
- 只使用与本项目原生 Vulkan 路径匹配的修复；没有引入 Venus 的共享内存绕行方案。

## 已修复的契约错误

1. BC/ASTC 精确格式与 feature 检查，保留 sRGB/SNORM；BC1 RGB 三色模式的第四个索引保持不透明。
2. 采样视图组合 NVN swizzle 与格式默认通道，RGBX 的 X 不再作为 alpha；附件使用独立、恒等 swizzle 的单层单 mip 视图。
3. 正确区分 1D、数组、3D、立方体；上传处理全部切片，移除单次数组上传的 16 层截断。
4. 压缩 mip 的 imageExtent 使用实际 texel 尺寸，允许贴图边缘的 1×1/2×2 mip；显式拒绝越界区域和不合法行距。
5. 打包贴图的源范围包含 Tegra GOB 填充，不能只用解排布后的线性字节数验证池边界。
6. Render pass 增加写入到后续读取的显式依赖；深度/模板 barrier 与布局匹配实际启用的 Vulkan 特性，保留模板内容。
7. 开启受支持的 independentBlend 等设备特性，不支持的状态明确拒绝；创建纹理失败后允许重试。
8. Present 的采样格式与 swapchain 格式不一致时补足 sRGB 转换；源 GLSL 和生成脚本纳入仓库。
9. OHOS 分包只读资源按大小写精确查找。此前在另一分包未命中时遍历同目录、逐项读取 xattr，真机采样反复落在 `readdir/lgetxattr`，导致启动加载极慢；保留点文件兼容和路径边界检查。
10. 默认关闭高频线程采样，诊断时可用 `DM_SAMPLE=2000:*`；分包位置仅由 RomFS 决定，更新原生库不会重新分配资源。
11. arena 正确向上对齐、完整比较缓存内容；常数顶点属性使用设备允许范围内的空闲 binding，详见上节。

## 这份游戏的实际资源结构

- 输入为用户本机提供的本体和 1.30.0 更新。更新 NCA 的签名、NPDM、PFS 和 RomFS 哈希层校验通过。
- RomFS 共 **21,306 文件、5,149,111,248 字节**。根目录有 `prefabs.xml`、`fonts`、`images`、`shaders_nx`，没有旧模板假定的 `data/.build.config`。
- 运行模块为 `main,sdk`，没有 `subsdk0`。主程序 build ID 为 `81b2acb640d82c9e688a72206a7b633c`。
- main BSS 为 3,274,313,288 字节。改写器将 BSS 保留为虚拟内存，在基址下方放置可达的跳板；修复了装载器负偏移边界相加溢出的问题。
- 禁用这个游戏不适用的 D2R 固定偏移驱动默认值（`DM_TITLE_DRV=0`）。
- ArkTS 根据生成的 manifest 拷贝模块及旁表，版本改变时刷新缓存。资源分到 entry/ds2 两个模块，每个约 2.6 GB。

解包经验：hactool 密钥过滤必须按完整键名匹配。仅保留部分 `*_source` 字段会让工具重新派生并覆盖正确的现成 key，可能出现“进程成功但资源内容错误”。本轮使用的筛选类别为完整的 `header_key` 和带两位十六进制后缀的 application/master/titlekek key。密钥值不写入源码或文档。

## 着色器

`scripts/build_dst_shaders.py` 从 68 个 `.ksh` 提取 **105 个唯一 shader stage**，
全部通过 Vulkan 1.1 SPIR-V 校验，输出 `build/game/shaders.bin`（805,432 字节）。

- key：`sha256(control[:0x838] + code[:min(total, 0x838)])[:8]`，与运行时一致。
- DST 使用从 c[2] 字节偏移 0x20 开始的 combined handle 表，每项 8 字节，NVN slot 为 `(handle - 8) / 2`。
- 不能沿用旧 D2R 的 separate texture/sampler 表偏移。
- UBO bank 3 映射 NVN UBO 0，bank 1 保存 shader 常量。
- 未识别的资源布局直接报错，避免生成静默采错槽位的库。

## 验证记录

| 检查 | 结果 | 本地产物 |
|---|---|---|
| Vulkan/纹理 CPU 检查，ASan/UBSan | 1,268 项通过，65,536 种顶点流占用组合通过 | `build/vk-compat-tests.log` |
| 生产上传函数故障注入 | 195 条断言通过 | 同上 |
| 小 BSS/3 GiB BSS、键页、跳板分支、OHOS RomFS 路径、动态缓冲复用与对齐 | 通过 | `build/dst-asset-tests.log` |
| 生产 shader loader 和 key 查找 | 105 项全部可读取、查找 | 同上 |
| OHOS arm64 原生库 | 编译链接成功 | `dm/ohos/build/libdmhost.so` |
| ArkTS 与完整资源分包 | 构建成功 | `build/hap/*/build/default/outputs/default/` |
| 真机独立 Vulkan 探针 | 未执行：设备拒绝执行 | `build/vk-device/device-signed-exact-run.log` |
| Windows 自动调试签名 | 成功；标准 hvigor 也已完成两个 SignHap | `build/hap-signed-build.log`、`build/hap-arena-build.log` |
| 完整 HAP 安装 | entry/ds2 均成功 | `build/device-fs-install.log`、`build/device-arena-install.log` |
| 真机启动、世界设置文字、shader 库加载 | Maleoon 910 / Vulkan 1.2；105 条库加载，BC 走软件解压；文字与边框对齐 | `build/device-evidence/` |
| 包内资源和运行模块一致性 | 21,306 个文件完整，shader/模块字节一致，无密钥容器 | `build/package-verification.json` |

用户重新登录 DevEco 后，官方自动签名服务已为 `com.rootrd.dontstarve` 生成匹配的调试 Profile。
Windows `hap-sign-tool.jar` 签名和安装均成功；将密码经 DevEco 官方工具加密后，标准
`hvigor assembleHap` 也成功。签名配置只保留在忽略的 `build/hap/`，明文密码临时文件已清理。
独立 ELF 探针仍被设备拒绝执行，不能把这项记成 GPU 读回测试通过。

## 排查与重建要点

- 先核验实际游戏版本、shader key 和资源布局，再复用其他游戏的适配：DST 的 combined handle / UBO ABI 与 D2R 不同。
- 先问设备具体格式、usage 和启用的 feature，BC 不支持就走匹配 sRGB/SNORM 的软件解码；不要将原始 BC 字节直接当 ASTC。
- 文字错位优先检查顶点/索引/UBO 的偏移和快照寿命；同地址、同长度、相同 dirty stamp 不足以证明内容相同。
- 抓图每次使用新的远端文件名。本轮发现重复覆盖同一 screenshot 路径可能收到旧图；只保留游戏前台的证据。
- 包内容必须再次核验。hvigor 会 strip 原生库，整文件 hash 不同并不代表库过期；本轮逐个比较 27 个 allocated ELF section，代码和数据均与最新构建一致。
- 两分包合计 21,306 个 RomFS 文件逐项核对文件名和大小；shader 与运行模块逐字节一致。

## 本地重建

源码模板保留在 `hap/`，签名和游戏资源只放到被 Git 忽略的 `build/hap/`。
以下步骤假定 `build/game/update/{ExeFS,RomFS}` 和 `build/game/patched/` 已就绪。

```powershell
python scripts/stage_dst_hap.py
$env:DEVECO_SDK_HOME = 'C:\Program Files\Huawei\DevEco Studio\sdk'
$env:JAVA_HOME = 'C:\Program Files\Huawei\DevEco Studio\jbr'
$env:Path = 'C:\Program Files\Huawei\DevEco Studio\tools\node;' + $env:Path
Set-Location build/hap
& 'C:\Program Files\Huawei\DevEco Studio\tools\ohpm\bin\ohpm.bat' install
& 'C:\Program Files\Huawei\DevEco Studio\tools\hvigor\bin\hvigorw.bat' --mode module -p product=default -p buildMode=debug assembleHap --no-daemon
```

在 DevEco 打开 `build/hap`，使用 Project Structure → Signing Configs 为
`com.rootrd.dontstarve` 自动生成调试签名，随后重复 assembleHap。
staging 脚本保留该目录已有的签名配置。首次需要安装两个 HAP；资源分配稳定且已核验 ds2 与当前 staging 一致后，仅更新原生库可构建并覆盖安装 entry。

Linux/WSL 回归：

```sh
VULKAN_INC="$NDK/sysroot/usr/include" python3 scripts/test_vk_compat.py
PYTHONPATH=build/tools/python python3 scripts/test_dst_assets.py --shaders build/game/shaders.bin
```

着色器重建：

```sh
python3 scripts/build_dst_shaders.py --romfs build/game/update/RomFS \
  --output build/game/shaders.bin --dotnet build/tools/dotnet/dotnet \
  --translator build/dotnet/bin/dmsh_xlate/release/dmsh_xlate.dll \
  --spirv-cross /tmp/dst-vulkan-tools/usr/bin/spirv-cross \
  --spirv-val /tmp/dst-vulkan-tools/usr/bin/spirv-val
```

## 真机仍需验证

签名、安装、启动画面和中文世界设置界面已验证，用户复测反馈正常。仍需继续覆盖世界内的不同地形、角色和建筑，以及长时间运行和独立 GPU 读回。
尚需结合实际调用确认 textureView 的 mip/layer/format/swizzle 覆盖、存储复用后的 swizzle 更新、
稀疏 MRT slot 等路径。当前实现尚未完整支持这些语义，离线通过不能排除它们造成的缺纹理。
禁止依据历史计数器或“没有日志”直接认定某张纹理没有上传；使用 `TEXTURE-DIAGNOSTICS.md` 中的关联事件。
