#!/bin/bash
# 把 nvn_vulkan.c 的行尾从被误改的 LF 恢复成仓库原本的 CRLF（用户硬规则：保留 C 文件原有 CRLF）。
set -e
F=/mnt/g/ds-harmony/dm/gfx/nvn/nvn_vulkan.c
before_cr=$(tr -cd '\r' < "$F" | wc -c)
before_l=$(wc -l < "$F")
# 先确保没有残留 \r，再逐行补 \r，得到纯 CRLF
sed 's/\r$//; s/$/\r/' "$F" > "$F.dos" && mv "$F.dos" "$F"
after_cr=$(tr -cd '\r' < "$F" | wc -c)
after_l=$(wc -l < "$F")
echo "改前 CR=$before_cr 行=$before_l  ->  改后 CR=$after_cr 行=$after_l"
# 期望：CR 与行数相等（每行一个 CRLF），且行数不变
[ "$after_cr" = "$after_l" ] && echo "OK 每行都是 CRLF" || echo "不一致，需检查"
