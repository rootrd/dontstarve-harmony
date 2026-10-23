#!/bin/bash
# 产物离线审计（装机前必做）：每张核三件事 —— 大小没变、数据区之前的头一字节没动、数据区确实变了。
# Offline artifact audit: size unchanged, header prefix byte-identical, payload actually rewritten.
IN=$1
OUT=$2
n=0; bad=0; same=0
while IFS=$'\t' read -r rel bytes ps blocks psnr rmse preset; do
  [ "$rel" = "rel" ] && continue
  [ -z "$rel" ] && continue
  s="$IN/$rel"; d="$OUT/$rel"
  n=$((n+1))
  if [ ! -f "$d" ]; then echo "缺产物 $rel"; bad=$((bad+1)); continue; fi
  a=$(stat -c %s "$s"); b=$(stat -c %s "$d")
  if [ "$a" != "$b" ] || [ "$a" != "$bytes" ]; then echo "大小变了 $rel $a vs $b（记账 $bytes）"; bad=$((bad+1)); continue; fi
  if ! cmp -s -n "$ps" "$s" "$d"; then echo "头被动过 $rel（前 $ps 字节）"; bad=$((bad+1)); continue; fi
  if cmp -s "$s" "$d"; then echo "数据区没变 $rel"; same=$((same+1)); fi
done < "$OUT/bake.tsv"
echo "核过 $n 张：不合格 $bad，整张未变 $same"
