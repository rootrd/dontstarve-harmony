#!/bin/bash
# 全量产物的账：转换数、拒绝按原因归类、PSNR 分布（这是装机前的产物自审，不看屏幕也能知道错没错）
# Pre-install artifact audit: counts, refusal reasons grouped, PSNR distribution.
for T in entry ds2; do
  S=/mnt/g/ds-astc/bake-$T
  echo "===== bake-$T"
  head -1 "$S/summary.txt"
  echo "-- 拒绝按原因"
  awk -F'\t' '/^拒/{print $3}' "$S/summary.txt" | sed 's/[0-9]\+/N/g' | sort | uniq -c | sort -rn | head -8
  echo "-- PSNR 分布（每图）"
  awk -F'\t' 'NR>1{v=$5; if(v<mn||mn==0)mn=v; if(v>mx)mx=v; s+=v; n++; if(v<30)low++; if(v<36)l36++}
    END{printf "张数%d 最低%.2f 最高%.2f 均值%.2f 低于30dB %d 低于36dB %d\n", n, mn, mx, s/n, low+0, l36+0}' "$S/bake.tsv"
  echo "-- alpha 通道 RMSE 最大 5 张"
  awk -F'\t' 'NR>1{print $6"\t"$5"\t"$1}' "$S/bake.tsv" | sort -rn | head -5
  echo "-- 块数合计 / 字节合计"
  awk -F'\t' 'NR>1{b+=$4; s+=$2}END{print b" 块  "s" 字节"}' "$S/bake.tsv"
done
