#!/bin/bash
# Launch, drive EULA -> main menu -> create world -> character select -> in world, and
# screenshot at the interesting points.
#
# The coordinates are the on-screen controller overlay in the device's NATIVE panel space
# (2848x1276 here). Find your own with `hdc shell uitest uiInput click X Y` and a
# screenshot; they are not portable across resolutions or overlay layouts.
#
#   TAG=...   output directory name under OUTDIR
#   OUTDIR=   where screenshots land (default ./out)
#   HDC=      path to hdc
#   PKG=      bundle name
set -u
HDC=${HDC:-hdc}
PKG=${PKG:-com.example.dontstarve}
TAG=${TAG:-run}
OUTDIR=${OUTDIR:-./out}
# The app-visible sandbox path is NOT readable over hdc. Use the shell-side view.
SB_SHELL=/data/app/el2/100/base/$PKG/haps/entry/files
mkdir -p "$OUTDIR/$TAG"

tapA() { "$HDC" shell "uitest uiInput click 2572 893" >/dev/null; }
tapB() { "$HDC" shell "uitest uiInput click 2406 1058" >/dev/null; }
tapP() { "$HDC" shell "uitest uiInput click 1549 200"  >/dev/null; }   # "+"
shot() { "$HDC" shell snapshot_display -f /data/local/tmp/x.jpeg >/dev/null; "$HDC" file recv /data/local/tmp/x.jpeg "$OUTDIR/$TAG/$1" >/dev/null; }

"$HDC" shell aa force-stop "$PKG" >/dev/null
"$HDC" shell aa start -a EntryAbility -b "$PKG" \
  --ps DM_MODULES main,sdk,subsdk0 --ps DM_IMPORTS 1 --ps DM_FS 1 --ps DM_TITLE_DRV 0 \
  --ps DM_SAMPLE 2000 --ps DM_THRDUMP 0 --ps DM_SPUR 500 \
  --ps DM_TEXDUMP 0 --ps DM_RBIMG 0 --ps DM_CRASHDIR "$SB_SHELL" --ps DM_KBD MyWorld \
  --ps DM_MISSLIST "$SB_SHELL/miss.txt" >/dev/null
echo "started, pid=$("$HDC" shell pidof $PKG | tr -d '\r ')"

sleep 110                       # boot + first paint
for k in 1 2 3 4 5 6 7 8; do "$HDC" shell "uitest uiInput click 640 1060" >/dev/null; sleep 0.3; done
tapA; sleep 6                   # accept the EULA
tapA; sleep 6                   # continue offline
tapB; sleep 6                   # dismiss the sign-in dialog
tapP; sleep 6                   # main menu -> world list
tapA; sleep 6                   # yes, create a game
tapP; sleep 5; tapB; sleep 4; tapA; sleep 6   # empty panel -> back -> first entry -> presets
tapA; sleep 6                   # Survival
tapA; sleep 6                   # Solo
tapP; sleep 90                  # "+" starts generation; it takes minutes, not seconds
echo "after generation, pid=$("$HDC" shell pidof $PKG | tr -d '\r ')"
shot worldgen.jpeg
tapA; sleep 20                  # pick a character -> enter
shot in_world.jpeg
echo "done -> $OUTDIR/$TAG"

# Read back the forensic files. The healthy result for miss.txt is that it does not exist.
"$HDC" shell "ls -l $SB_SHELL" 2>/dev/null | tail -10
for f in miss.txt thr.txt; do
  "$HDC" file recv "$SB_SHELL/$f" "$OUTDIR/$TAG/$f" >/dev/null 2>&1 && echo "got $f" || echo "no $f (expected for miss.txt)"
done
"$HDC" shell "ls $SB_SHELL" 2>/dev/null | tr -d '\r' | grep -o 'crash_[0-9]*\.txt' | while read -r c; do
  "$HDC" file recv "$SB_SHELL/$c" "$OUTDIR/$TAG/$c" >/dev/null && echo "got $c"
done
