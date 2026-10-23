#!/bin/bash
# 在 WSL 里编 bc2astc（astcenc 已在这儿编好）| build bc2astc inside WSL, where astcenc is already built
set -e
A=${ASTCENC_SRC:-/root/fmt_probe_astc/src}
R=${REPO:-/mnt/g/ds-harmony}
mkdir -p /mnt/g/ds-astc/build
cd "$R/dm/tools"
echo "astcenc 头：$(ls "$A/Source/astcenc.h")"
gcc -O2 -std=gnu11 -Wall -I"$A/Source" -o /mnt/g/ds-astc/build/bc2astc \
    bc2astc.c ../gfx/tex/nvn_bcdec.c "$A/build/Source/libastcenc-native-static.a" \
    -lstdc++ -lm -lpthread
ls -l /mnt/g/ds-astc/build/bc2astc
