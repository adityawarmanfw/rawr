#!/bin/sh
# Run CloneActivity, dump the camera service mid-run, pull results, check key 40. usage: clone.sh [am start extras...]
P=${P:-android.camera.cts}
F=/sdcard/Android/data/$P/files/clone
OUT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)/tmp/clone"
mkdir -p "$OUT"; rm -rf "$OUT"/*
adb shell am force-stop com.android.camera
adb shell am start -S -n $P/com.rawr.probe.CloneActivity "$@" >/dev/null
sleep ${D1:-3}; adb shell dumpsys media.camera > "$OUT/dump_preview.txt"
sleep ${D2:-3}; adb shell dumpsys media.camera > "$OUT/dump_still.txt"
for i in $(seq 30); do adb shell grep -q END $F/meta.txt 2>/dev/null && break; sleep 1; done
adb pull $F/. "$OUT/" >/dev/null 2>&1
cat "$OUT/meta.txt"
for d in dump_preview dump_still; do
  awk '/Device [0-9]+ is open/{f=1} f&&/Latest received frame/{g=1} g&&/Device dump:/{exit} g' "$OUT/$d.txt" > "$OUT/${d}_res.txt"
  echo "-- $d"; grep -A1 -E "SensorCurrentMode|ExposureCount \(|NumberOfExpFrames|AECLinearGain|SensorVirtualChannelInfo|DCGMode \(" "$OUT/${d}_res.txt" | grep -v '^--' | paste - - | sed 's/  */ /g'
done
