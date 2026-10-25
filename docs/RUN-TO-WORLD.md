# 跑到地图:装机后能真正进世界的那条路(2026-10-24 验证版)

写给要用自己方法打包的协作者。**能装上 ≠ 能进世界**——本文记的是 10-24 当天反复验证过、
能一路开到世界里的完整流程,以及三处"少一步就进不去"的硬门槛。

## 0. 两条打包路线的关系

| 路线 | 做法 | 适用 |
|---|---|---|
| 路线 A(本文):换 .so 重打 entry | HAP 壳工程(`hap/` 模板对应的本地工程)里已装好 5.18 GB 游戏资源(entry + ds2 双模块),每轮只换 `libdmhost.so` 重打 entry 模块 | **日常迭代**。5 分钟一轮,`ds2` 不动 |
| 路线 B:[stage_dst_hap.py](../scripts/stage_dst_hap.py) 从解包产物拼全新 HAP | 首次装配/换资源 | 首次装机 |

两条路线的**启动参数是同一套**(§3)。参数不对,哪条路线都进不了世界——这正是"装上了却在
世界门口卡死"的头号原因。

## 1. 硬门槛一:启动参数不是可选项

游戏必须用 `aa start --ps` 带参启动。**桌面图标点进去 = 所有开关全关**,后果实测:

- **没有 `DM_TITLE_DRV 0`**:宿主带着 D2R(暗黑 2)的固定偏移驱动默认值跑饥荒,行为未定义。
- **没有 `DM_RT_ASTC`(+ 目录)**:BC3 贴图只剩 CPU 软解一条路,实测**世界整层全黑**
  (交接记录 §5:软解只能当诊断旁路)。马良 920 一个 BC 块格式都不认,世界地表全靠
  设备端 BC3→ASTC 现编,**这是能看见地图的充要条件**。
- 没有日志开关:崩了无从取证。

能进世界的最小参数集(当天验证过的那份,直接抄):

```bash
aa start -a EntryAbility -b com.rootrd.dontstarve \
  --ps DM_TITLE_DRV 0 \
  --ps DM_RT_ASTC 60 \
  --ps DM_RT_ASTC_DIR /data/storage/el2/base/haps/entry/files/astc \
  --ps DM_LOGFILE /data/storage/el2/base/haps/entry/files/hlog.txt \
  --ps DM_SAMPLE 1000:*
```

加料轮(取证)的完整模板见仓库外 `params_<TAG>` 文件的用法:把整串 `--ps ...` 放进一个文件,
`PARAMS="$(cat params_X)"` 后拼进 `aa start`。常用可选开关:`DM_NVN_TSV`(逐帧调用计数)、
`DM_SPKEY`(点名追踪)、`DM_TEXDUMP`/`DM_TEXREAD`(贴图落盘/读回),全表见
[DM-SWITCHES.md](DM-SWITCHES.md)。

## 2. 硬门槛二:大 HAP 不能用 `hdc install -r`

entry 的带签名包约 2.2 GB,`hdc install -r` 会**截断并假报 verify signature failed**。
必须两步:

```bash
export MSYS_NO_PATHCONV=1     # Git Bash 会把 /data/... 改写成 Windows 路径,必须关
hdc -t <序列号> file send entry-default-signed.hap /data/local/tmp/e.hap
hdc -t <序列号> shell "bm install -r -p /data/local/tmp/e.hap"
```

`ds2` 资源模块装过一次就不用再动;只换 .so 的轮次只重打/重装 entry。

## 3. 硬门槛三:锁屏状态 `aa start` 直接报 10106102

开发者模式不会自动解锁。启动前:解锁屏幕、`power-shell timeout -o 2147483647` 压住息屏,
否则 8 分钟的加载中途熄屏,一轮作废。

## 4. 路线 A 全流程(当天复现过 N 次)

1. **编 .so**(构建机 aarch64):全量同步 `dm/` 树(别只推单文件——邻居新功能没同步就是
   dlopen 失败),`rm -rf dm/ohos/build/pic && make -f dm/ohos/Makefile solib`。
   三道门槛:未定义 `_Z` 符号数 = 0;剩余未定义全为 WEAK;新加的标记字符串真的在二进制里。
2. **换块**:`libdmhost.so` 放进 HAP 工程的 `entry/libs/arm64-v8a/`(记 md5)。
3. **打包**:无头 hvigor,`PATH` 要有 JDK/DevEco 自带 jbr 的 `bin` + `tools/node` +
   `tools/hvigor/bin`:
   `node <DevEco>/tools/hvigor/bin/hvigorw.js --mode module -p product=default assembleHap --no-daemon`
   产物:`entry/build/default/outputs/default/entry-default-signed.hap`(自动调试签名)。
4. **包内自审**:hvigor 会**剥符号**,所以"包内 .so md5 == 源 md5"是无效断言;正确做法是
   断言标记字符串能在源 .so 里 grep 到 + 包内比源小。`shaders.bin` 是 resfile 不剥,可逐字节
   md5 断言。
5. **装机**:§2 的两步。
6. **带参启动**:§1 的最小参数集 + 解锁(§3)。
7. **进世界**(全程用 `@draws` 判断位置,别靠截图猜;`@draws` = 客体每帧真实提交的绘制数,
   从 `DM_NVN_TSV` 的 tsv 里 grep):

| 阶段 | @draws 指纹 | 动作 |
|---|---|---|
| 标题页 | 12–14 | A(开始游戏) |
| 世界列表 | 24–44 | ddown 选中存档 → a |
| 世界设置面板 | 84–149 | **plus(=启动服务器;别按 a,焦点在名称上会弹软键盘)** |
| 生成世界 | 142 附近停几分钟,"正在生成世界:N%"在走 | 等满 |
| 选角页 | 620–660 | a 进衣柜 → **dup(把焦点从皮肤格移开)** → plus(出发!) |
| **世界里** | **700+** | 等 ≥150 秒(开场是夜里,30 秒就拍会误判全黑)+ 走动一次再下结论 |

夜里地面发黑是游戏机制不是故障(交接记录里两次误判都栽在这:夜判黑、只比夜里的门)。
白天 + 噪斑才是当前 open blocker。

## 5. 虚拟手柄按键坐标

`entry/src/main/ets/model/PadLayout.ets` 的归一化布局 × 屏幕 2848×1276:

```
a=0.903,0.700  b=0.845,0.829  x=0.845,0.571  y=0.787,0.700
dup=0.225,0.582  ddown=0.225,0.818  dleft=0.172,0.700  dright=0.278,0.700
plus=0.544,0.157  minus=0.456,0.157
```

注入:`hdc shell uitest uiInput click <X> <Y>`;走动:`uitest uiInput swipe 239 545 380 480 900`(左摇杆往右上推)。

## 6. 每一步都烧过一轮的仪器陷阱(速查)

- 截图 `snapshot_display -f` **不覆盖同名文件**:每次换文件名,否则拿回旧图误判卡死。
- 判画面是否在动:隔 3 秒两张对比 md5;判跑到哪:`@draws`。
- `%{public}` 只能出现在 `OH_LOG_Print` 的格式串里,塞进 snprintf 会把日志打花。
- `[vk]` 前缀日志进 `DM_LOGFILE`,`[dm]` 前缀只进 hilog;取错地方会以为"什么都没打"。
- 多 GB 的 hilog 别直接 grep/tail(超时),拉回 PC 解析;设备上没有 python/awk。
- 仓库行尾逐文件不同(`nvn_vulkan.c`/`app_ohos.c` 是 CRLF),工具重写会造整文件假 diff。

——以上每条的完整背景在 [HANDOFF-2026-10-24.md](HANDOFF-2026-10-24.md) §4/§8。
