#!/bin/bash
# 三个「几何不吻合」的到底是哪种头：把它们的头、尺寸、体积和数据区推算一并摊开
# Show the three geometry outliers: header bytes, dims, size, and the predicted payload.
for T in entry ds2; do
  awk -F'\t' '/^拒/ && $3 ~ /几何/ {print $2}' /mnt/g/ds-astc/bake-$T/summary.txt | while read -r rel; do
    f=/mnt/g/ds-hap/$( [ $T = entry ] && echo entry || echo ds2 )/src/main/resources/resfile/romfs/$rel
    echo "--- bake-$T $rel"
    xxd -l 16 "$f"
    stat -c "体积 %s" "$f"
  done
done
