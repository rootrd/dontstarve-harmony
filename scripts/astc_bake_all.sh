#!/bin/bash
# 全量转换：两层 romfs 各出一份产物树，先 entry 后 ds2（覆盖层在同名时才是赢家）
# Full bake: one output tree per romfs layer; ds2 runs second because the overlay wins on name collisions.
set -e
B=/mnt/g/ds-astc/build/bc2astc
E=/mnt/g/ds-hap/entry/src/main/resources/resfile/romfs
D=/mnt/g/ds-hap/ds2/src/main/resources/resfile/romfs
P=${PRESET:-medium}
J=${JOBS:-6}
rm -rf /mnt/g/ds-astc/bake-entry /mnt/g/ds-astc/bake-ds2
mkdir -p /mnt/g/ds-astc/bake-entry /mnt/g/ds-astc/bake-ds2
echo "== entry 层 $(date '+%F %T')"
"$B" --in "$E" --out /mnt/g/ds-astc/bake-entry --jobs "$J" --preset "$P"
echo "== ds2 层 $(date '+%F %T')"
"$B" --in "$D" --out /mnt/g/ds-astc/bake-ds2 --jobs "$J" --preset "$P"
echo "== 完 $(date '+%F %T')"
