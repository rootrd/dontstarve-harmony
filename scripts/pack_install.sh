#!/bin/bash
# Build the HAP, self-audit the artefact, transfer, verify sizes, install.
#
# Why the two assertions exist: an incorrect package does not fail loudly. It installs the
# PREVIOUS libdmhost.so, runs fine, and you spend a whole device round measuring nothing.
#
#   MARK=...      a string literal that only this round's source contains (required)
#   HDC=...       path to hdc
#   HDC_TARGET=... explicit device target (required)
#   PREBUILT=1    skip the hvigor step (reuse whatever is already built)
#
# Usage: MARK="排队上传 s=" scripts/pack_install.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
HAP=${HAP:-$ROOT/build/hap}
HDC=${HDC:-hdc}
HDC_TARGET=${HDC_TARGET:?set HDC_TARGET from hdc list targets}
MARK=${MARK:?set MARK to a runtime string literal from this round's source - not a comment, comments are stripped}
SO="$HAP/entry/libs/arm64-v8a/libdmhost.so"
EO="$HAP/entry/build/default/outputs/default"
DO="$HAP/ds2/build/default/outputs/default"
DEVDIR=/data/local/tmp/dstapp

[ -e "$SO" ] || { echo "red: no $SO - run scripts/build_host.sh first"; exit 1; }
SOT=$(stat -c %Y "$SO")

if [ "${PREBUILT:-0}" != 1 ]; then
  # hvigorw is a Node script that shells out to Java: a JDK must be on PATH.
  ( cd "$HAP" && node "$HVIGORW" --mode module -p module=entry@default -p product=default \
      -p buildMode=debug assembleHap --no-daemon 2>&1 | tail -3 )
  ( cd "$HAP" && node "$HVIGORW" --mode module -p module=ds2@default -p product=default \
      -p buildMode=debug assembleHap --no-daemon 2>&1 | tail -3 )
fi

HAPF="$EO/entry-default-signed.hap"
S=$(wc -c < "$HAPF")
echo "signed entry hap = $S B"
[ "$S" -gt 1000000000 ] || { echo "red: implausibly small - not signed, or the romfs is missing"; exit 1; }

# Assert 1: the hap must not be older than the .so (hvigor really rebuilt).
HT=$(stat -c %Y "$HAPF")
[ "$HT" -ge "$SOT" ] || { echo "red: hap ($(date -d @$HT)) older than so ($(date -d @$SOT)) - nothing rebuilt"; exit 1; }

# Assert 2: the .so INSIDE the hap must carry this round's marker.
# Do not compare md5s: hvigor strips symbols before packaging (4.4 MB -> 1.5 MB), so a
# perfect package fails an md5 check every time. Stripping removes symbols, not string
# literals, so grep for a log line instead.
unzip -p "$HAPF" libs/arm64-v8a/libdmhost.so > /tmp/inhap.so || { echo "red: no such .so inside"; exit 1; }
grep -a -q "$MARK" /tmp/inhap.so || { echo "red: the packaged .so lacks \"$MARK\" - it is a stale object"; exit 1; }
echo "green: packaged .so carries this round's marker ($(wc -c < /tmp/inhap.so) B after stripping)"

# Verify both module identities and SHA-256 hashes, then replace them together.
# Updating keeps the existing saves. Never uninstall as part of a build/deploy.
python3 "$HERE/install_dst_hap.py" --project "$HAP" --hdc "$HDC" --target "$HDC_TARGET"
