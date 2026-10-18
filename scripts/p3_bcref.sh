#!/bin/bash
# P3 第一档判据的地基：离线那份 BC 参考解码器（scripts/p3_join.py 的 ref_decode）必须与装机那份（dm/gfx/tex/nvn_bcdec.c
# 走的 bcdec）逐字节一致。这条不对拍，「CPU 已坏」这一档就可能是我们自己判错的——一整轮真机证据会白拿。
# Byte-for-byte equivalence between the offline reference decoder used by the P3 join and the decoder that ships on the
# device. Without this gate the "CPU side is already broken" rung could be our own artifact.
#
# 跑法 | run:  bash scripts/p3_bcref.sh      （宿主 cc + python3，不需要设备）
#   PY=... CC=... 可覆盖
set -u
cd "$(dirname "$0")/.." || exit 1
CC=${CC:-cc}
PY=${PY:-python3}
D=$(mktemp -d) || exit 1
trap 'rm -rf "$D"' EXIT

command -v "$PY" >/dev/null 2>&1 || { echo "【红】本机没有 python3（构建机上有）"; exit 1; }
"$CC" -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -o "$D/bcref" \
    dm/gfx/nvn/tests/bcref_equiv.c dm/gfx/tex/nvn_bcdec.c || { echo "【红】对拍程序没编过"; exit 1; }

n=0; bad=0
for f in 0x41 0x42 0x43 0x44 0x45 0x46 0x47 0x48; do
    for wh in "8 8" "13 7" "1 1" "64 64" "16 3"; do
        set -- $wh
        "$D/bcref" "$f" "$1" "$2" "$D/bl.bin" "$D/bc.rgba" > "$D/said" 2>"$D/err" || {
            echo "【红】0x$f ${1}x${2} 生产侧没解：$(head -1 "$D/err")"; bad=$((bad+1)); continue; }
        "$PY" scripts/p3_join.py --refdump "$f" "$1" "$2" "$D/bl.bin" "$D/py.rgba" >/dev/null 2>"$D/err" || {
            echo "【红】0x$f ${1}x${2} 离线侧没解：$(head -1 "$D/err")"; bad=$((bad+1)); continue; }
        if cmp -s "$D/bc.rgba" "$D/py.rgba"; then
            n=$((n+1))
        else
            echo "【红】0x$f ${1}x${2} 两份解码结果不同：$(cmp "$D/bc.rgba" "$D/py.rgba" | head -1)"
            bad=$((bad+1))
        fi
    done
done
echo "== 对拍：同 $n 组｜红 $bad 组"
[ $bad = 0 ] && echo "【绿】离线参考解码器与生产解码器逐字节一致——P3 的 CPU 档可以用" || echo "【红】先修离线那份解码器，再谈像素分界"
exit $((bad ? 1 : 0))
