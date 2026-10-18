#!/bin/bash
# Compile the host into libdmhost.so and drop it into the HAP project.
#
# The cross toolchain lives on a Linux box in practice (an AArch64 Linux + OHOS sysroot
# setup is much less painful on Linux than on Windows), so this script ships the sources
# there, builds, and brings the .so back. Everything is overridable by environment
# variable; nothing is hard-coded to anybody's machine.
#
#   BOX=user@buildhost          where clang + $(HOME)/ohos/sysroot live
#   REPO=~/work/dontstarve      checkout on that box
#   HAP=../hap                  local HAP project (receives the .so)
#
# Usage: scripts/build_host.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BOX=${BOX:-user@buildhost}
REPO=${REPO:-~/work/dontstarve}
HAP=${HAP:-$ROOT/hap}
LIBDIR="$HAP/entry/libs/arm64-v8a"

cd "$ROOT"
# Only what git says changed. A tar of the whole tree hides the fact that you forgot to
# commit something, and the build box then keeps compiling a stale file you thought you
# had fixed.
{ git diff --name-only; git ls-files --others --exclude-standard; } \
  | grep -E '^dm/' | grep -vE '^dm/ohos/build/' | sort -u > /tmp/dirty.txt
echo "syncing $(wc -l < /tmp/dirty.txt) changed source files"
tar czf /tmp/dmsync.tgz -T /tmp/dirty.txt
scp -q /tmp/dmsync.tgz "$BOX:/tmp/"

# One ssh call, not several: back-to-back ssh/scp invocations against this box have been
# observed to interrupt each other.
ssh "$BOX" "cd $REPO && tar xzf /tmp/dmsync.tgz && make -f dm/ohos/Makefile solib 2>&1 | tail -20 && ls -l dm/ohos/build/libdmhost.so"

mkdir -p "$LIBDIR"
scp -q "$BOX:$REPO/dm/ohos/build/libdmhost.so" "$LIBDIR/"
echo "installed -> $LIBDIR/libdmhost.so ($(wc -c < "$LIBDIR/libdmhost.so") B)"
