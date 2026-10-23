#!/bin/bash
# 三张单级文件（旧版被几何拒收）：现在应走 geo=单 回退并转成
B=/mnt/g/ds-astc/build/bc2astc
E=/mnt/g/ds-hap/entry/src/main/resources/resfile/romfs
rm -rf /mnt/g/ds-astc/t1geo
for f in SplashScreen.tex images/fepanels.tex images/wave_shadow.tex; do
  out=/mnt/g/ds-astc/t1geo/$(basename "$f" .tex)
  mkdir -p "$out"
  echo "=== $f"
  "$B" --in "$E" --out "$out" --only "$f" --jobs 1 --preset "${PRESET:-medium}" || true
  head -4 "$out/summary.txt" 2>/dev/null || true
  head -2 "$out/bake.tsv" 2>/dev/null || true
done
