#!/bin/bash
# 收一轮 P3 取证现场并先自证产物可信：hdc 取回采集目录 → 与 p3_manifest.tsv 逐项对齐（在位/字节数/整幅哈希）→
# 两份清单合成一张 → 交给 scripts/p3_join.py 出分界。
# Fetch one P3 capture round, audit every artifact against its manifest line BEFORE it is used as evidence, merge the
# two capture directories into one view, and print the join command.
#
# 只取回、只判：不装包、不删设备上的东西（指令：设备侧不许用卸载重装清场）。
#
#   HDC=...   path to hdc            TEX=/…/tex  RD=/…/rd   LOG=/…/vk.log   TAG=名字   OUT=./out
#   这两个目录要和 --ps DM_TEXDUMP / --ps DM_TEXREAD 指的完全一致；同一个目录也行（那就不用合）。
set -u
HDC=${HDC:-hdc}
PKG=${PKG:-com.rootrd.dontstarve}
SB=/data/app/el2/100/base/$PKG/haps/entry/files
TEX=${TEX:-$SB/tex}
RD=${RD:-$SB/rd}
LOG=${LOG:-$SB/vk.log}
TAG=${TAG:-p3-$(date +%m%d-%H%M)}
OUT=${OUT:-./out}
D="$OUT/$TAG"
mkdir -p "$D" || exit 1
command -v "$HDC" >/dev/null 2>&1 || { echo "【红】找不到 hdc（HDC=/完整路径 再跑）"; exit 1; }

echo "== 设备侧在位情况"
"$HDC" shell "ls -l $TEX 2>/dev/null | tail -3; echo -n 'tex 文件数 '; ls -1 $TEX 2>/dev/null | wc -l; echo -n 'rd 文件数 '; ls -1 $RD 2>/dev/null | wc -l; ls -l $LOG 2>/dev/null" 2>&1 | tr -d '\r'

# 取回：整目录优先（hdc 新版递归），不成的话逐文件收，绝不静默少收
pull() {   # pull <设备目录> <本地子目录>
    local remote=$1 loc=$2
    mkdir -p "$loc"
    if "$HDC" file recv "$remote" "$loc" >/dev/null 2>&1; then
        [ -d "$loc/$(basename "$remote")" ] && mv "$loc/$(basename "$remote")"/* "$loc/" 2>/dev/null
    fi
    if [ ! -f "$loc/p3_manifest.tsv" ]; then
        echo "   （整目录没收到，改逐文件收）"
        "$HDC" shell "ls -1 $remote" 2>/dev/null | tr -d '\r' | while read -r f; do
            [ -n "$f" ] && [ "$f" != "." ] && [ "$f" != ".." ] && "$HDC" file recv "$remote/$f" "$loc/$f" >/dev/null 2>&1
        done
    fi
    ls -1 "$loc" 2>/dev/null | wc -l | sed 's/^/   收到 /;s/$/ 个文件/'
}
pull "$TEX" "$D/tex"
[ "$TEX" != "$RD" ] && pull "$RD" "$D/rd"
"$HDC" file recv "$LOG" "$D/run.log" >/dev/null 2>&1 && echo "   日志 $(wc -c < "$D/run.log") 字节" || echo "   【提示】日志没收到（$LOG 不在位？带 --ps DM_LOGFILE=1 才有）"
if [ "${SHOT:-1}" != 0 ]; then
    "$HDC" shell snapshot_display -f /data/local/tmp/p3.jpeg >/dev/null 2>&1
    "$HDC" file recv /data/local/tmp/p3.jpeg "$D/screen.jpeg" >/dev/null 2>&1 && echo "   屏幕截图已收" || echo "   【提示】截图没收到"
fi

# 自检：清单每行的文件在位、字节数一致、整幅哈希一致。错产物会静默作废整轮真机测试，所以这一步不许跳。
# Audit first: a truncated or overwritten artifact must be caught here, not mistaken for a pixel verdict later.
M=0
merge_manifest() {   # merge_manifest <本地目录>
    local loc=$1
    [ -f "$loc/p3_manifest.tsv" ] || { echo "【红】$loc 没有 p3_manifest.tsv：这半边等于没采到"; return 1; }
    local head; head=$(head -1 "$loc/p3_manifest.tsv" | tr -d '\r')
    [ "$head" = "$(printf 'kind\tserial\tstamp\tmip\tlayer\tw\th\tpitch\tbytes\tsha256\tnonzero\tdraws\tpresents\tpath')" ] || {
        echo "【红】$loc 的清单表头与生产不一致，列序不能信"; return 1; }
    local miss=0 bad=0 n=0
    while IFS=$'\t' read -r kind serial stamp mip layer w h pitch bytes sha nz draws pres path; do
        [ "$kind" = "kind" ] && continue
        n=$((n+1))
        local f="$loc/$path"
        if [ ! -f "$f" ]; then echo "   缺文件 $path"; miss=$((miss+1)); continue; fi
        local sz got
        sz=$(wc -c < "$f" | tr -d ' ')
        if [ "$sz" != "$bytes" ]; then echo "   尺寸不符 $path 清单$bytes 实际$sz"; bad=$((bad+1)); continue; fi
        got=$(sha256sum "$f" | cut -d' ' -f1)
        [ "$got" = "$sha" ] || { echo "   哈希不符 $path（取回截断或同名覆盖过）"; bad=$((bad+1)); }
    done < "$loc/p3_manifest.tsv"
    echo "   $loc：清单 $n 行｜缺 $miss｜哈希或尺寸错 $bad"
    [ $((miss + bad)) = 0 ] || { echo "【红】这半边的产物不可信：先修取证，再谈像素"; return 1; }
    tail -n +2 "$loc/p3_manifest.tsv" | tr -d '\r' >> "$D/merged/p3_manifest.tsv"
    cp -f "$loc"/*.bin "$D/merged/" 2>/dev/null
    M=$((M + n))
    return 0
}
mkdir -p "$D/merged"
: > "$D/merged/p3_manifest.tsv"
printf 'kind\tserial\tstamp\tmip\tlayer\tw\th\tpitch\tbytes\tsha256\tnonzero\tdraws\tpresents\tpath\n' > "$D/merged/p3_manifest.tsv"
ok=1
merge_manifest "$D/tex" || ok=0
[ "$D/rd" != "$D/tex" ] && { merge_manifest "$D/rd" || ok=0; }

echo
echo "== 收样完毕：$D（清单 $M 行，合到 $D/merged）"
[ $M = 0 ] && { echo "【红】一张产物都没有：开关值必须是绝对目录（不是 1），且不能是存档目录"; exit 1; }
if [ "$ok" = 1 ]; then
    echo "【绿】产物自检全过——可以拿它下像素结论"
else
    echo "【红】产物自检有红：按指令这轮只能报「取证缺陷」，不许报像素分界"
fi
PY=${PY:-python3}
# 两个分支写成同一句是死代码，读的人会以为没 python3 时另有安排——只留一条真判据。
command -v "$PY" >/dev/null 2>&1 || echo "   【提示】本机没有 python3：把 $D 整个送到构建机上跑同一句（scripts 已随仓库同步）"
RUN="$PY scripts/p3_join.py $D/merged"
echo "   下一步（一次只判一个分界，先钉一个 serial）："
echo "     $RUN --serial <serial> --fmt <NVN格式号> --log $D/run.log"
