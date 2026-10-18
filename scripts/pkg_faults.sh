#!/bin/bash
# 打包上传这一路的故障注入验收：从生产源码里 awk 抽出 vktex/计数器/队列票据/暂存环整段/统计映射表/退避
# 和 pkg_upload、pkg_flush、pkg_queue、tex_new_life，再加上复活那几行与延迟销毁（坟场）整段，
# 用本机 cc 编 dm/gfx/nvn/tests/pkg_faults.c 跑断言——被测的就是装机那一份代码，桩只负责让某一格失败。
# Extract the production bodies, compile them against the injectable stubs in the test file, and assert the matrix.
# NEG=<git-ref> runs the same assertions against an older version of the file: those must go red,
# otherwise the assertions do not bite the defects they claim to cover.
set -u
cd "$(dirname "$0")/.." || exit 1

SRC=${1:-dm/gfx/nvn/nvn_vulkan.c}
NEG=${NEG:-}
[ "$NEG" = 1 ] && NEG=HEAD~1          # 裸 NEG=1 沿用老写法：对上一版提交判红 | bare NEG=1 means "the previous commit"
# 只借 sysroot 的 vulkan 头；整棵 include 塞进 -I 会让 musl 的 stdint 盖掉 glibc
VULKAN_INC=${VULKAN_INC:-$HOME/ohos/sysroot/usr/include}
CC=${CC:-cc}

D=$(mktemp -d) || exit 1
trap 'rm -rf "$D"' EXIT
cp dm/gfx/nvn/tests/pkg_faults.c $D/test.c || exit 1

if [ -n "$NEG" ]; then
    git show "$NEG:$SRC" > $D/old.c || { echo "【红】取不出 $NEG:$SRC"; exit 1; }
    RAW=$D/old.c; MODE="旧版 $NEG"
else
    RAW=$SRC; MODE="工作区新版"
fi
[ -f "$RAW" ] || { echo "【红】源码不在位：$RAW"; exit 1; }

# 先清成「LF + 没有 %{public}」的副本：终止判据要数大括号，格式串里的 %{} 会捣乱
# Normalize first (strip CRLF and %{public}) because the extractor counts braces to find each end of function.
sed 's/%{public}/%/g; s/\r//g' "$RAW" > $D/clean.c || exit 1
S=$D/clean.c
cd $D || exit 1
OLD=0; [ -n "$NEG" ] && OLD=1

# vktex 定义：先定位 } vktex;，再往前找回它那一行 typedef struct {（文件里这种行有一大把，只能这么钉）
awk '/^\} vktex;$/{e=NR} {L[NR]=$0} END{if(!e){exit 3} for(i=e;i>0;i--) if(L[i] ~ /^typedef struct \{$/){for(j=i;j<=e;j++)print L[j]; exit}}' "$S" > body.inc
grep -q '^} vktex;' body.inc || { echo "【红】没抓到 vktex 定义"; exit 1; }

# 计数器那一行（跨两行，以分号收尾）
awk '/^static _Atomic uint64_t g_pkg_up/{f=1} f{print} f&&/;[ \t]*$/{exit}' "$S" >> body.inc
# 拒绝分档的枚举（新代码才有，定义在文件头部）
[ "$OLD" = 0 ] && grep -a '^enum { STG_OK' "$S" >> body.inc
# 统计映射表 + pkg_stat_line（新代码才有：标签和计数器写死在同一行，测试手抄一张对着干）
if [ "$OLD" = 0 ]; then
  awk '/^typedef struct \{ const char \*lab;/{f=1} f{print; if(!t && /^};$/){t=1} else if(t && /^}$/){exit}}' "$S" >> body.inc
  grep -q '^static int pkg_stat_line' body.inc || { echo "【红】没抓到统计映射表"; exit 1; }
fi
# 队列：#define PKG_PEND 到 static int g_pkg_pend_n;（新旧两版都是这一段，新代码中间多 born）
awk '/^#define PKG_PEND/{f=1} f{print} f&&/^static int g_pkg_pend_n;$/{exit}' "$S" >> body.inc
echo '' >> body.inc
# 暂存环整段：BIG_STAGE_SZ → mtl_sub_reset_bigstage（含 pkg_stage/bigs_build/乘加判据/stage_begin/stage_take）
# 这段新旧都在位，抽出来才算真被测：旧版那句 g_bigs_off + need > cap 会绕回，就靠这段判红。
awk '/^#define BIG_STAGE_SZ/{f=1} f{print} f&&/^void mtl_sub_reset_bigstage\(void\) \{ g_bigs_off = 0; \}$/{exit}' "$S" >> body.inc
grep -q 'static uint64_t stage_take' body.inc || { echo "【红】暂存环没抽到 stage_take"; exit 1; }
grep -q 'void mtl_sub_reset_bigstage' body.inc || { echo "【红】暂存环整段截早了"; exit 1; }
[ "$OLD" = 0 ] && grep -a '^enum { PKG_UP_RETRY' "$S" >> body.inc
# 函数体：只认「定义」（下一行是单独一行的 {），终止靠数大括号——同行收尾的 }} 也数得平
FNS="pkg_upload pkg_flush pkg_queue tex_new_life"
[ "$OLD" = 0 ] && FNS="pkg_backoff $FNS"
for fn in $FNS; do
  awk -v p="$fn" '
    !f && index($0,"static ")==1 && index($0,p){getline nx; if(nx ~ /^\{[ \t]*$/){print; print nx; f=1; d=1} next}
    f{ a=gsub(/\{/,"{"); b=gsub(/\}/,"}"); print; d=d+a-b; if(d<=0) exit }' "$S" >> body.inc
  echo '' >> body.inc
done
# 复活那几行：从「v->serial = t->serial; v->d = *t;」抓到紧随的 tex_new_life(v);
awk '/v->serial = t->serial; v->d = \*t;/{f=1} f{print} f&&/tex_new_life\(v\);/{exit}' "$S" | sed 's/^        //' > revive.inc
# 延迟销毁整段：从 grave_t 那行 typedef 抓到 grave_reap 的收尾大括号（旧版没有这一段，NEG 时留空文件）
awk '/grave_t;$/{f=1} f{print; if(!rp && $0 ~ /^static void grave_reap/) rp=1; else if(rp && /^\}$/) exit}' "$S" > grave.inc
echo "抽出：body $(wc -l < body.inc) 行｜复活 $(wc -l < revive.inc) 行｜坟场 $(wc -l < grave.inc) 行｜$MODE"

# 每个需要的名字都必须在 body.inc 里以「定义」出现且只出现一次：出现两次就是终止判据数错串进了下一个定义
REQ="stage_begin stage_take bigs_build"
[ "$OLD" = 0 ] && REQ="$REQ pkg_stat_line"
for f in $FNS $REQ; do
  n=$(grep -cE "^static [^ ]+( [^ ]+)? $f\(" body.inc)
  [ "$n" = 1 ] || { echo "【红】$f 在 body.inc 里命中 $n 次（应为 1 个定义）"; grep -n " $f(" body.inc; exit 1; }
done
grep -q 'g_pkg_pend\[g_pkg_pend_n++\]' body.inc || { echo "【红】pkg_queue 抽成前向声明了"; exit 1; }
# 坟场这段要么整段在位（新代码），要么整段缺席（旧版）：半截会让 C4-3 断言测到手写的桩
if [ "$OLD" = 0 ]; then
  for f in tex_retire grave_reap grave_kill; do
    grep -q "^static void $f(" grave.inc || { echo "【红】坟场没抽到 $f"; exit 1; }
  done
  grep -q 'g_grave\[g_grave_n++\]' grave.inc || { echo "【红】坟场抽漏了入坟那一行"; exit 1; }
  grep -q 'GRAVE_N' grave.inc || { echo "【红】坟场没带上界定义"; exit 1; }
else
  [ ! -s grave.inc ] || { echo "【红】旧版里竟有坟场段，抽错了源"; exit 1; }
fi

if [ "$OLD" = 0 ]; then
  # 提交生命周期两条静态判据（这两段牵扯整层设备对象，搬不进宿主桩，就在这里按源码结构钉死）：
  # ① 重入闸门必须在 mtl_sub_reset_draw 之前——闸门在后就是「先把游标清了才发现缓冲还开着」，已录 copy 的字节会被覆盖
  awk '/^void mtl_sub_begin\(void\)/{f=1}
       f&&/g_sub_reenter/{r=1}
       f&&/mtl_sub_reset_draw\(/{if(!r){print "【红】sub_begin 的重入闸门排在 reset_draw 之后：游标会被清掉，已录拷贝的字节会被覆盖"; exit 1} exit 0}
       f&&/^\}$/{print "【红】sub_begin 里没有重入闸门"; exit 1}' "$S" || exit 1
  # ② 四个 vk 返回值都必须接住判掉（位 256 那条是「游戏提交整个不上 GPU」的诊断旁路，不算）
  awk '/^uint64_t mtl_sub_commit\(void\)/{f=1} f{print} f&&/^\}$/{exit}' "$S" > commit.inc
  for p in "VkResult er = vkEndCommandBuffer" "VkResult cr = vkQueueSubmit" "VkResult wr = vkWaitForFences" "VkResult rr = vkResetFences"; do
    [ "$(grep -Fc "$p" commit.inc)" = 1 ] || { echo "【红】mtl_sub_commit 少了「$p」这条返回值判据"; exit 1; }
  done
  echo "静态判据：sub_begin 闸门在位｜提交四条返回值都接住"
fi

mkdir -p inc
for d in vulkan vk_video KHR; do [ -d "$VULKAN_INC/$d" ] && cp -r "$VULKAN_INC/$d" inc/; done
[ -d inc/vulkan ] || { echo "【红】拿不到 vulkan 头（VULKAN_INC=$VULKAN_INC）"; exit 1; }
FLAGS="-std=gnu11 -O0 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Iinc"
if [ "$OLD" = 1 ]; then
  # 旧版没有退避/分档那几段，ASan 只会把桩自己的历史噪声算进来；判据是「同一套断言要变红」
  $CC $FLAGS -DOLD -o pkg_faults test.c 2> err.txt || { echo "【红】旧版没编过："; head -30 err.txt; exit 1; }
else
  $CC $FLAGS -fsanitize=address,undefined -o pkg_faults test.c 2> err.txt || { echo "【红】新代码没编过："; head -40 err.txt; exit 1; }
fi
echo "编译告警 $(grep -c 'warning:' err.txt) 条"
./pkg_faults
rc=$?
if [ "$OLD" = 1 ]; then
  [ "$rc" != 0 ] && echo "【绿】反例成立：旧版被同一套断言判红（退出码 $rc）" || echo "【红】反例失败：旧版全绿 = 这些断言没咬到缺陷"
else
  [ "$rc" = 0 ] && echo "【绿】新代码全绿" || echo "【红】新代码有失败，见上"
fi
exit $rc
