#!/bin/sh
# Pull the newest DCG sweep (SweepActivity) into tmp/dcg_sweep/ and stop the probe. usage: pull_dcg.sh [pkg]
set -e
P=${1:-android.camera.cts}
F=/sdcard/Android/data/$P/files
RUN=$(adb shell "ls -d $F/dcg_* 2>/dev/null" | tr -d '\r' | sort | tail -1)
[ -n "$RUN" ] || { echo "no dcg_* run on the device"; exit 1; }
adb shell am force-stop $P
OUT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)/tmp/dcg_sweep"
mkdir -p "$OUT"
adb pull "$RUN" "$OUT/" >/dev/null
echo "$OUT/$(basename "$RUN")"; cat "$OUT/$(basename "$RUN")/rows.txt"
