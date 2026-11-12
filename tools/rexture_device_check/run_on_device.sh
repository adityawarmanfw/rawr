#!/bin/sh
# Runs rexture_device_check on a connected phone (adb). Needs the NDK build (see CMakeLists.txt) and rawburst folders
# (bundle.txt, frame_NN.raw16, frame_NN.meta).
#   run_on_device.sh <binary> <out dir> check <burst dir>...      stage dumps to <out>/<burst>/ for the NumPy comparison
#   run_on_device.sh <binary> <out dir> time  <burst dir> <noise a,b>   Rexture vs HDR+ Quality, 8 and 16 frames
# No camera or UI is touched; the app is force-stopped before and after so it cannot compete for the GPU.
set -eu
bin=$1; out=$2; mode=$3; shift 3
dev=/data/local/tmp/rx
adb shell "am force-stop com.rawr.camera; am force-stop com.rawr.camera.benchmark" >/dev/null 2>&1 || true
adb shell "mkdir -p $dev" && adb push "$bin" $dev/rexture_device_check >/dev/null && adb shell "chmod +x $dev/rexture_device_check"
mkdir -p "$out"
adb shell getprop ro.product.model > "$out/model.txt"; adb shell dumpsys battery | grep -i "temperature" >> "$out/model.txt" || true
case "$mode" in
check)
  for b in "$@"; do
    n=$(basename "$b")
    adb shell "rm -rf $dev/$n" && adb push "$b" $dev/ >/dev/null
    ab=$(python3 -I -c "import json;m=json.load(open('$b/meta.json'));print('%r,%r'%(m['noise_a'],m['noise_b']))")
    adb shell "cd $dev && rm -rf out_$n && ./rexture_device_check $n --noise-ab $ab --dump-stages out_$n/stages" | tee "$out/$n.log"
    mkdir -p "$out/$n" && adb pull $dev/out_$n/stages "$out/$n/" >/dev/null
    adb shell "rm -rf $dev/out_$n"
  done ;;
time)
  b=$1; ab=$2; n=$(basename "$b")
  adb shell "rm -rf $dev/$n" && adb push "$b" $dev/ >/dev/null
  : > "$out/time.log"
  for frames in 8 16; do
    for m in rexture hdrplus-freq; do
      echo "== $m frames=$frames" | tee -a "$out/time.log"
      adb shell "cd $dev && ./rexture_device_check $n --merge $m --frames $frames --noise-ab $ab --repeat 4" | tee -a "$out/time.log"
      sleep 20   # let the SoC cool between runs
    done
  done ;;
*) echo "unknown mode $mode" >&2; exit 2 ;;
esac
adb shell "am force-stop com.rawr.camera; am force-stop com.rawr.camera.benchmark" >/dev/null 2>&1 || true
