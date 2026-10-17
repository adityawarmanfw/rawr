#!/bin/sh
# one capture: one.sh <pkg> <cam> <row> <w> <h> ; prints meta, pulls into tmp/fullres_probe/<pkg>/
P=$1; F=/sdcard/Android/data/$P/files; OUT="$(dirname "$0")/../../tmp/fullres_probe/$P"; mkdir -p "$OUT"
adb shell "rm -rf $F/cam*"
adb shell am start -S -W -n $P/com.rawr.probe.ProbeActivity --es mode capture --es cam $2 --ei row $3 --ei w $4 --ei h $5 --ei frames 1 --ez lease ${LEASE:-true} --ez auto ${AUTO:-false} --el exp ${EXP:-33000000} --ez vivokeys ${VIVO:-false} --ei still ${STILL:-0} ${ISO:+--ei iso $ISO} ${WARMUP:+--ei warmup $WARMUP} ${AF:+--ez af $AF} ${FOCUS:+--ef focus $FOCUS} ${KEYS:+--es keys $KEYS} ${USAGE:+--el usage $USAGE} >/dev/null
for i in $(seq 40); do sleep 1; adb shell "grep -q END $F/cam*/meta.txt" 2>/dev/null && break; done
adb shell "cat $F/cam*/meta.txt" | grep -v -E 'capture failed|^result [1-9]' | cut -c1-200
adb pull $F/. "$OUT/" >/dev/null 2>&1
adb shell am force-stop $P
