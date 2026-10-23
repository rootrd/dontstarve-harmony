#!/bin/bash
# 三张钉过的文件先各跑一遍：两张应转（设备报过 NVN 0x44），一张必须拒（NVN 0x42 = 8 字节块）
# Smoke test on the three device-proven files: two BC3 must convert, the BC1 one must be refused.
set -e
B=/mnt/g/ds-astc/build/bc2astc
E=/mnt/g/ds-hap/entry/src/main/resources/resfile/romfs
D=/mnt/g/ds-hap/ds2/src/main/resources/resfile/romfs
P=${PRESET:-medium}
rm -rf /mnt/g/ds-astc/t1
for spec in "E levels/tiles/dock_falloff.tex" "E levels/textures/ocean_noise.tex" "D images/button_icons.tex"; do
  root=${spec%% *}
  only=${spec#* }
  [ "$root" = E ] && IN=$E || IN=$D
  out=/mnt/g/ds-astc/t1/$(basename "$only" .tex)_$P
  mkdir -p "$out"
  echo "=== $only  档位 $P"
  "$B" --in "$IN" --out "$out" --only "$only" --jobs 1 --preset "$P" || true
  head -5 "$out/summary.txt" 2>/dev/null || true
  head -3 "$out/bake.tsv" 2>/dev/null || true
done
