# Headless phone performance checks

Live VF telemetry (opens the app; only run when the user allows foreground use):

```sh
uv run --no-project tools/phone-performance/app_preview_benchmark.py \
  --seconds 25 --output tmp/phone-performance/live-preview
```

This preserves image settings, captures native GPU timing intervals and saves
UI/gfx/thermal/memory reports. It stops the app in `finally`. A run without a
complete interval is rejected. Compare matching recipes/scenes; native GPU
timestamps and UI gfx counters do not measure sensor-to-display latency.

## Actual app benchmark (opens the camera: obtain user go-ahead first)

Build `:app:assembleBenchmark` for an R8-optimized, non-debuggable, debug-signed
`com.rawr.camera.benchmark` APK with test tags. It installs alongside the user's
app. Use `UI_PKG=com.rawr.camera.benchmark scripts/ui` for all interactions;
Settings deep links are not exported in this variant, so navigate by test tags.
Full app timing is enqueue-to-successful publication, measured with one monotonic
clock. It includes provider work and processing but not any self-timer before enqueue.

```sh
uv run --no-project tools/phone-performance/app_capture_benchmark.py \
  --variant segmented --warmups 3 --samples 20 --output tmp/phone-performance/app-run
```

Configure the same recipe/scene before paired runs. `--variant contiguous` and
`--variant segmented` affect standard JPEG only; UltraHDR uses its contiguous mux
path in both cases. Do not attribute UltraHDR timing to the segmented writer.
The runner restores `debug.rawr.jpeg_stream`, stops the app on exit and rejects
process exits/timeouts. A missing summary marks an incomplete run.

Debug A/B controls (restore after testing):

* `debug.rawr.cached_readback=0` selects the original coherent readback memory;
  default prefers compatible cached memory with explicit invalidation.
* `debug.rawr.mali_fair_queue=1` opts into the experimental equal local priorities
  on Mali. Restart the app to apply. It is **not enabled by default**: the first
  long stress run hit device loss and further validation requires app access.
  Adreno local/global priority policy is unchanged.
* `debug.rawr.raw_content_stats=1` enables the expensive once-per-second live RAW
  CPU-lock diagnostic. Normal preview skips it; RAW capture/statistics algorithms
  used for image processing remain unchanged.
* `debug.rawr.single_primary_queue=1` opts single-frame demosaic/render work into
  preview's primary queue and matching mutex, frozen per capture. It remains an
  experiment, defaults off and does not change HDR+/MFSR scheduling. Restore the
  original property after a paired run. Initial Mali foreground captures passed;
  physical Adreno and a controlled same-input queue A/B remain pending.

Actual Home/resume validation (opens the app; always stops it on exit):

```sh
uv run --no-project tools/phone-performance/app_lifecycle_benchmark.py \
  --cycles 3 --gesture-home --output tmp/phone-performance/home-run
```

It uses `scripts/ui home --gesture` for system navigation and tagged controls
for the app. Reports include same-process resume, camera closure four seconds
after Home, foreground/Home PSS, launcher gfx/framestats and optional per-surface
presentation data. Whole-run launcher jank percentages can include unrelated
frames; use frame-timeline traces to attribute stalls. UI-ready observations
include automation overhead and are not animation latency. Launcher gfx collection
currently targets this phone's `com.miui.home`.

`app_capture_benchmark.py --home-during-capture --samples 1 --warmups 0` tests
publication after shutter immediately followed by the Home gesture.
`--home-after-last` checks memory and camera closure after completed publication.
Both flags are recorded so background runs cannot be mistaken for foreground
capture benchmarks. These tests create normal gallery captures and retain them.

Headless cached-readback visibility/performance check, with an installed Android
NDK compiler (API 33, ARM64):

```sh
"$RAWR_NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android33-clang++" \
  -std=c++17 -O2 -static-libstdc++ -Wall -Wextra -Wpedantic -Werror \
  -Iapp/src/main/cpp -Inative/vk_common/testing \
  tools/phone-performance/readback_check.cpp -lvulkan -o tmp/phone-performance/readback_check_android
adb push tmp/phone-performance/readback_check_android /data/local/tmp/
adb shell chmod 755 /data/local/tmp/readback_check_android
adb shell /data/local/tmp/readback_check_android
```

The check fills a buffer on the GPU, records transfer-to-host visibility, waits
on a fence, invalidates cached/noncoherent memory and verifies exact bytes over
three patterns for both memory policies. It requires no app or camera.

These tools exercise production native code without opening the camera. JPEG
benchmarks measure CPU encoding/output to `/dev/null` and process peak RSS;
they do **not** measure shutter-to-saved, MediaStore, camera scheduling, FUSE,
or complete application/GPU memory. File output is an additional untimed iteration.
`run-device-vulkan.sh` measures native RAW preview compute timestamps; it is not
a measurement of displayed VF FPS or camera-to-display latency.

## Host checks

Use the repository's bundled libjpeg-turbo and the same compiler/options for
baseline and candidate. Generated artifacts stay under `tmp/`:

```sh
mkdir -p tmp/phone-performance/baseline
git show 4c64cb1:app/src/main/cpp/encoding/jpeg/StripJpegEncoder.cpp > tmp/phone-performance/baseline/StripJpegEncoder.cpp
git show 4c64cb1:app/src/main/cpp/encoding/jpeg/StripJpegEncoder.h > tmp/phone-performance/baseline/StripJpegEncoder.h
cmake -S native/libjpeg-turbo -B tmp/phone-performance/ljt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=0 -DWITH_TOOLS=0 -DWITH_TESTS=0
cmake --build tmp/phone-performance/ljt --target turbojpeg-static --parallel 4
cmake -S tools/phone-performance -B tmp/phone-performance/host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTURBOJPEG_LIBRARY="$PWD/tmp/phone-performance/ljt/libturbojpeg.a" \
  -DRAWR_JPEG_BASELINE_DIR="$PWD/tmp/phone-performance/baseline"
cmake --build tmp/phone-performance/host --parallel 4
ctest --test-dir tmp/phone-performance/host --output-on-failure
tmp/phone-performance/host/jpeg_benchmark_baseline flat 4080 3064 98 1 20 tmp/phone-performance/baseline.jpg
tmp/phone-performance/host/jpeg_benchmark segments 4080 3064 98 1 20 tmp/phone-performance/candidate.jpg
cmp tmp/phone-performance/baseline.jpg tmp/phone-performance/candidate.jpg
```

Subsampling IDs come from TurboJPEG: 0=444, 1=422, 2=420, 3=gray. The tool
reports the arithmetic median and nearest-rank p95. It prints individual measured
samples to stderr and a JSON summary to stdout. Run variants as separate processes
so `getrusage` high-water RSS is independent. Input texture is deterministic.

Configure another tools build with `-DRAWR_SANITIZERS=ON` for ASan/UBSan. The
strip check compares decoded pixels to single-thread TurboJPEG, checks streamed
bytes against contiguous output, and rejects every possible failing sink position.
It covers 95/98/100 quality, four subsampling modes, odd/aligned dimensions, 1080p,
12.5MP, 50MP and the small-image fallback. The writer check runs real metadata,
async ownership, write failure, reset and UltraHDR mux code. Host adapters replace
only Android logcat and property access. Android builds use the real Android APIs.

## Android build and ADB execution

Set `RAWR_NDK` to an installed NDK. The initial native-only benchmark used NDK
27.0.12077973 and API 33, identically for both binaries; this does not satisfy the
full app's pinned NDK 29 build check.

```sh
RAWR_NDK="$ANDROID_HOME/ndk/27.0.12077973"
cmake -S native/libjpeg-turbo -B tmp/phone-performance/ljt-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$RAWR_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=0 -DWITH_TOOLS=0 -DWITH_TESTS=0
cmake --build tmp/phone-performance/ljt-android --target turbojpeg-static --parallel 4
cmake -S tools/phone-performance -B tmp/phone-performance/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$RAWR_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release \
  -DTURBOJPEG_LIBRARY="$PWD/tmp/phone-performance/ljt-android/libturbojpeg.a" \
  -DRAWR_JPEG_BASELINE_DIR="$PWD/tmp/phone-performance/baseline"
cmake --build tmp/phone-performance/android --parallel 4
adb shell mkdir -p /data/local/tmp/rawr-phone-performance
adb push tmp/phone-performance/android/jpeg_benchmark_baseline /data/local/tmp/rawr-phone-performance/jpeg_bench_baseline_android
adb push tmp/phone-performance/android/jpeg_benchmark /data/local/tmp/rawr-phone-performance/jpeg_bench_candidate_android
adb push tmp/phone-performance/android/jpeg_writer_check /data/local/tmp/rawr-phone-performance/jpeg_writer_check_android
adb push tools/phone-performance/run-device-native.sh /data/local/tmp/rawr-phone-performance/
adb shell 'chmod 755 /data/local/tmp/rawr-phone-performance/*_android'
adb shell /data/local/tmp/rawr-phone-performance/jpeg_writer_check_android
adb shell sh /data/local/tmp/rawr-phone-performance/run-device-native.sh
adb pull /data/local/tmp/rawr-phone-performance/results tmp/phone-performance/device-results
uv run --no-project tools/phone-performance/summarize.py tmp/phone-performance/device-results
```

The device script uses ABBA ordering and records thermal state, battery, available
memory, exact build fingerprint, raw samples, and byte-equivalent saved JPEGs.
Do not run other benchmarks at the same time. Retain separate result directories
for reruns. Report CPU frequency/load variation rather than treating it as speedup.

For Vulkan baseline/validation, build the existing standalone module and stage its
four executables plus SPIR-V into `raw-preview-device`:

```sh
cmake -S native/raw_preview -B tmp/phone-performance/raw-preview-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$RAWR_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build tmp/phone-performance/raw-preview-android --parallel 4
mkdir -p tmp/phone-performance/raw-preview-device
cp tmp/phone-performance/raw-preview-android/raw_preview*.spv tmp/phone-performance/raw-preview-device/
cp tmp/phone-performance/raw-preview-android/raw_preview_benchmark tmp/phone-performance/raw-preview-device/
cp tmp/phone-performance/raw-preview-android/raw_preview_cfa_state_benchmark tmp/phone-performance/raw-preview-device/
cp tmp/phone-performance/raw-preview-android/raw_preview_validate tmp/phone-performance/raw-preview-device/
cp tmp/phone-performance/raw-preview-android/raw_preview_cfa_state_validate tmp/phone-performance/raw-preview-device/
adb push tmp/phone-performance/raw-preview-device /data/local/tmp/rawr-phone-performance/
adb push tools/phone-performance/run-device-vulkan.sh /data/local/tmp/rawr-phone-performance/
adb shell 'chmod 755 /data/local/tmp/rawr-phone-performance/raw-preview-device/raw_preview*'
adb shell sh /data/local/tmp/rawr-phone-performance/run-device-vulkan.sh
adb pull /data/local/tmp/rawr-phone-performance/raw-preview-device/results tmp/phone-performance/device-vulkan-results
```

Both scripts work with a selected `adb -s` device when invoking ADB; set
`ANDROID_SERIAL` for the repeated commands above. Neither script modifies app
preferences, installs APKs, requires root, or leaves a camera session running.
Application lifecycle/UI checks still use `scripts/ui` per `AGENTS.md` and require
an awake unlocked device. Repeat quality/performance checks on physical Adreno
hardware before declaring Mali/Adreno compatibility validated.
