#!/bin/bash
# 换装：把两层 romfs 里已转换的 .tex 用 ASTC 产物就地替换，原 BC3 先整体挪进备份树（可逆）。
# 只动 bake 清单里列出的文件；写一个 astc-baked 标记，防止对同一棵树重复转（转换后头仍是 0x20，光看头分不清 BC3/ASTC）。
# Swap: replace each converted .tex in the HAP romfs with its ASTC product, moving the original BC3 into a backup tree first.
# Touches only files listed in bake.tsv; drops an astc-baked marker so a tree is never double-baked (a converted file still
# carries bytes[4]=0x20, so header alone can't tell BC3 from ASTC).
set -e
PAIRS="entry ds2"
BAKE=/mnt/g/ds-astc
HAP=/mnt/g/ds-hap
for T in $PAIRS; do
  SRC="$BAKE/bake-$T"
  ROM="$HAP/$T/src/main/resources/resfile/romfs"
  BK="$BAKE/orig/$T"
  TS="$SRC/bake.tsv"
  [ -f "$TS" ] || { echo "缺清单 $TS"; exit 2; }
  if [ -f "$ROM/.astc-baked" ]; then echo "$ROM 已换装过（有 .astc-baked），跳过——先还原再重来"; continue; fi
  mkdir -p "$BK"
  n=0
  # 逐行读清单（跳过表头），rel=第1列
  tail -n +2 "$TS" | while IFS=$'\t' read -r rel rest; do
    [ -z "$rel" ] && continue
    [ -f "$SRC/$rel" ] || { echo "缺产物 $rel"; continue; }
    mkdir -p "$BK/$(dirname "$rel")" "$ROM/$(dirname "$rel")"
    # 原件挪出（若还没挪）
    [ -f "$ROM/$rel" ] && mv "$ROM/$rel" "$BK/$rel"
    cp "$SRC/$rel" "$ROM/$rel"
    n=$((n+1))
  done
  # 计数用清单行数（while 在子 shell 里，n 不回传）
  cnt=$(tail -n +2 "$TS" | grep -c . || true)
  printf 'baked-from=%s\ncount=%s\ndate=%s\n' "$SRC" "$cnt" "$(date '+%F %T')" > "$ROM/.astc-baked"
  # 第二份要打进安装包：宿主没拿到 DM_TEX_ASTC 时按它自动开（桌面图标启动没有 --ps 参数）。
  # hvigor 收 rawfile/resfile 会丢点文件，所以这个标记不带点。
  mkdir -p "$ROM/data"
  cp "$ROM/.astc-baked" "$ROM/data/astc_baked.txt"
  echo "$T 层：换装 $cnt 张，原件备份在 $BK"
done
echo "== 换装完。还原：把 orig/<层>/ 下的 .tex 拷回 romfs 同名位置，并删 .astc-baked 与 data/astc_baked.txt =="
