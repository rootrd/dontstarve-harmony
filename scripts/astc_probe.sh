#!/bin/bash
# 独立复核：拿构建机自带的 astcenc CLI 解一段产物负载，证明这些字节确实是合法 ASTC 4x4 LDR，
# 与我的 bc2astc 无关（不是"自己产、自己验"）。
set -e
CLI=/root/fmt_probe_astc/src/build/Source/astcenc-native
B=/mnt/g/ds-astc/bake-entry
E=/mnt/g/ds-hap/entry/src/main/resources/resfile/romfs
mkdir -p /tmp/astcprobe
# SplashScreen.tex：1280x720，头 18B，ASTC 负载 = 末尾 921600B
dd if="$B/SplashScreen.tex" of=/tmp/astcprobe/ss.astc bs=4096 count=921600 skip=18 iflag=skip_bytes,count_bytes status=none
echo "负载字节 $(stat -c %s /tmp/astcprobe/ss.astc)（应为 921600）"
# 16 字节 astcenc 原生 .astc 头：magic 13 AB A1 5C | 块 4x4x1 | dim 1280x720x1（各 3 字节小端）
printf '\x13\xab\xa1\x5c\x04\x04\x01\x00\x05\x00\xd0\x02\x00\x01\x00\x00' > /tmp/astcprobe/hdr.bin
cat /tmp/astcprobe/hdr.bin /tmp/astcprobe/ss.astc > /tmp/astcprobe/ss_full.astc
echo "带头文件 $(stat -c %s /tmp/astcprobe/ss_full.astc) 字节"
"$CLI" -dl /tmp/astcprobe/ss_full.astc /tmp/astcprobe/ss.png
echo "解出 PNG $(stat -c %s /tmp/astcprobe/ss.png) 字节"
echo "--- 原 BC3 头：宽@8 高@10（u16LE）---"
od -An -tu2 -j8 -N2 "$E/SplashScreen.tex"
od -An -tu2 -j10 -N2 "$E/SplashScreen.tex"
