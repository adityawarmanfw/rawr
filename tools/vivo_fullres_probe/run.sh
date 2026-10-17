#!/bin/sh
# usage: run.sh <cam> <row> <w> <h> [frames] [lease] [pkg]  -> pulls into tmp/fullres_probe/
set -e
P=${7:-android.camera.cts}
RUN="cam$1_row$2_$3x$4"; [ "${6:-true}" = false ] && RUN="${RUN}_nolease"
D=/sdcard/Android/data/$P/files/$RUN
adb shell rm -rf "$D"
adb shell am start -S -W -f 0x10008000 -n $P/com.rawr.probe.ProbeActivity --es mode capture --es cam "$1" --ei row "$2" --ei w "$3" --ei h "$4" \
  --ei frames "${5:-3}" --ez lease "${6:-true}" >/dev/null
for i in $(seq 1 30); do sleep 1; adb shell grep -q END "$D/meta.txt" 2>/dev/null && break; done
adb shell am force-stop $P
OUT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)/tmp/fullres_probe/$P"
mkdir -p "$OUT"; rm -rf "$OUT/$RUN"
adb pull "$D" "$OUT/" >/dev/null && cat "$OUT/$RUN/meta.txt"; ls -l "$OUT/$RUN" | tail -n +2
