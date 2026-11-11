#!/bin/sh
# Regression check of the Rexture grain pass on the phone (adb): the merge output must be bit-identical whatever the strip
# height of the pass is (the strips overlap by the filter reach; a seam bug shows up as a differing checksum).
#   strip_check.sh <rexture_device_check binary> <rawburst dir>
# Needs a burst with moving content and fallback areas (a street scene) so that the pass has work at the seams.
set -eu
bin=$1; burst=$2; n=$(basename "$burst"); dev=/data/local/tmp/rx_strip
ab=$(python3 -I -c "import json;m=json.load(open('$burst/meta.json'));print('%r,%r'%(m['noise_a'],m['noise_b']))")
adb shell "am force-stop com.rawr.camera" >/dev/null 2>&1 || true
adb shell "mkdir -p $dev" && adb push "$bin" $dev/check >/dev/null && adb shell "chmod +x $dev/check"
adb shell "test -d $dev/$n" || adb push "$burst" $dev/ >/dev/null
ref=
for rows in 256 100 48 16; do
  sum=$(adb shell "cd $dev && rm -rf d && ./check $n --merge rexture --noise-ab $ab --strip-rows $rows --dump-stages d >/dev/null 2>&1; md5sum d/final.bin | cut -d' ' -f1; rm -rf d" | tr -d '\r')
  echo "strip rows $rows: $sum"
  [ -z "$ref" ] && ref=$sum
  [ "$sum" = "$ref" ] || { echo "FAIL: output depends on the strip height" >&2; adb shell "rm -rf $dev"; exit 1; }
done
adb shell "rm -rf $dev"
echo "ok: output is independent of the strip height"
