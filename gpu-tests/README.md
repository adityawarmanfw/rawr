# Headless Android Vulkan test APK

A dedicated instrumentation target, following RawLens's native device-test
approach, without launching RAWR. The manifest contains no activities or launcher
entry. GPU tests do not access a camera; the separately selected camera suite
uses CAMERA permission and a camera foreground service with a notification. Target package:
`com.rawr.gputest`; runner package: `com.rawr.gputest.test`.

The JNI bridge builds **production sources**, not a reimplementation: RAW preview,
highlight reconstruction, tone engine, and their existing validation/benchmark
harnesses. Shaders are built by the pinned NDK toolchain and embedded into the test
library. No generated SPIR-V or APK belongs in Git. Native code uses release
optimization even though the instrumentable target is a debug-signed APK.

```sh
./gradlew :gpu-tests:assembleDebug :gpu-tests:assembleDebugAndroidTest
adb install -r gpu-tests/build/outputs/apk/debug/gpu-tests-debug.apk
adb install -r gpu-tests/build/outputs/apk/androidTest/debug/gpu-tests-debug-androidTest.apk
adb shell am instrument -w -r \
  -e class com.rawr.gputest.HeadlessGpuTest \
  com.rawr.gputest.test/androidx.test.runner.AndroidJUnitRunner
mkdir -p tmp/phone-performance/headless-apk-results
adb exec-out run-as com.rawr.gputest tar -cf - files/gpu-results \
  > tmp/phone-performance/headless-apk-results/reports.tar
tar -xf tmp/phone-performance/headless-apk-results/reports.tar \
  -C tmp/phone-performance/headless-apk-results
```

Use `-e class com.rawr.gputest.HeadlessGpuTest#cachedReadbackExactBytes` for one
case. The full suite covers four Bayer phases, production RGB geometries, exact
sensor clip classification, cached/coherent CPU visibility, GPU parity against
the original CST arithmetic, timing for every built-in AgX preset and direct output transform, and
both RAW preview highlight methods. `summary.txt` records wall duration and
**post-test process PSS**, not GPU peak allocation or app capture peak memory.
`native.txt` contains GPU timestamps, reference errors and threshold results.

The preview harness prints `FAIL` for its hardcoded 1 ms timing target even when
its executable returns zero. JUnit success means execution/correctness assertions
passed; inspect native timing thresholds separately. Non-neutral tone cases are
performance runs, not CPU parity claims. No new tone/reconstruction arithmetic
is introduced by the harness.

The older CPU tone oracle models legacy controls and is not used to assert
current renderer parity. The GPU comparison checks the current controls with
the original CST implementation; it is not a photographic quality assessment.

Run the real camera suite explicitly:

```sh
adb shell pm grant com.rawr.gputest android.permission.CAMERA
adb shell pm grant com.rawr.gputest android.permission.POST_NOTIFICATIONS
adb shell am instrument -w -r \
  -e class com.rawr.gputest.HeadlessCameraTest -e samples 3 \
  com.rawr.gputest.test/androidx.test.runner.AndroidJUnitRunner
```

It selects a public RAW-capable sensor (override with `-e cameraId 0`), captures
the largest available RAW_SENSOR size, matches image/result timestamps and saves
fsynced DNG fixtures. After closing the camera it replays the first DNG through
the **actual production benchmark renderer library**, then validates JPEG size
and decoding. Default recipe: RCD, opposed highlights, FCC=2, defringe, JPEG98
4:2:2. Override `-e demosaic Amaze` or disable replay with
`-e productionReplay false`. Samples must be 1–100. Fixtures remain app-private.

Publication creates a test-owned pending MediaStore row, copies/fsyncs the
rendered JPEG, publishes it, checks size and decoding, and deletes that row in
`finally`. Its copy/publication timing is separate from replay timing. This tests
Android provider behaviour, **not RAWR's live still coordinator or its exact
publication implementation**. Camera/session/reader/service cleanup runs in
`finally`. Normal Android permission/privacy restrictions still apply.

Currently excluded: full film/LUT/denoise matrix, physical display latency,
Activity lifecycle and capture-to-gallery end-to-end timing. The opt-in native
preview test below covers Camera2/Vulkan presentation and native stop/resume.
Actual displayed VF latency and Home-gesture behaviour still need an app test
and user go-ahead. Repeat on physical Adreno hardware as well as Mali.

Keep benchmarks serial and record battery/thermal/driver context. Test reports
are local to this target and do not inspect or change the user's camera data.
The JNI bridge redirects stdout/stderr and current directory only while holding
one serial test lane in this dedicated test process.

Cold single-frame replay (no camera access, requires the captured fixture):

```sh
adb shell am instrument -w -r \
  -e class com.rawr.gputest.RendererReplayTest -e label baseline-fresh-1 \
  -e samples 3 -e demosaic Rcd -e highlightMethod 1 -e profile RawrBase \
  com.rawr.gputest.test/androidx.test.runner.AndroidJUnitRunner
```

Use a fresh alphanumeric/hyphen/underscore label for every run. Each sample
copies the same DNG to a distinct path so its linear cache starts empty. The
reported duration includes open, render, fsync and close, excluding fixture
copy and full-image decoded ARGB SHA-256 calculation. Hashes compare decoded
pixels across APK versions without EXIF timing differences. Post-close PSS is
not peak memory. Dimensions currently match this phone's 4080×3060 fixture;
this test deliberately asserts those dimensions. Profile options include
`RawrBase`, `SRgb`, `Rec709`, `AgX`; highlight method is 0 or 1. This is DNG
replay, not the production live capture coordinator, HDR+ or MFSR.

`HeadlessGpuTest#previewToneBypassSwitching` checks three frame slots starting
with bypass, switching both ways, and compares exact recovered linear bytes
with a bypass-only run. Native timing targets remain separate from JUnit pass.

Production preview/lifecycle without opening an Activity:

```sh
adb shell pm grant com.rawr.gputest android.permission.CAMERA
adb shell am instrument -w -r \
  -e class com.rawr.gputest.NativePreviewTest -e detachSurface true \
  -e cycles 6 -e profile 2 \
  com.rawr.gputest.test/androidx.test.runner.AndroidJUnitRunner
```

This uses the real Camera2/Vulkan engine, the shipped zero-copy default and
built-in camera profile. A SurfaceTexture on an offscreen EGL thread drains
presentation; there is no Activity, launcher, overlay or visible camera UI.
The camera foreground-service notification remains visible while running.
Each cycle waits for at least 30 consumed buffers, stops the native preview,
optionally detaches presentation, allows one second to drain and checks that
no buffers arrive during another second. It records running/stopped PSS,
stop/detach duration and process CPU time over that second. Run `cycles` 2–10;
profile IDs are 0 RawrBase, 2 sRGB, 3 Rec709, 9 AgX (other built-in IDs 4–8,10
are accepted; imported LUT ID1 needs a separate fixture). `-e zeroCopy false`
selects the legacy copy path. `detachSurface` defaults false.

This covers the native surface/lifecycle boundary, not Compose, the Home gesture,
SurfaceFlinger display latency, active recording or still-during-background
behaviour. Callback counts can coalesce and are not sensor-to-display latency.
PSS includes test/EGL overhead. Logs/reports use unique `native-preview-*`
directories. HDR+ and MFSR are not enabled by this test.

For longer production-camera profiling, add `-e frames 240 -e waveform true
-e profile 0`. `frames` accepts 30–600 consumed buffers per cycle; the per-cycle
deadline is 60 seconds. `waveform` enables the RGB waveform using the real scope
engine. Frame counts include consumer callback coalescing; the reported
`startToTargetFramesMs` includes setup and is not a steady display FPS estimate.
GPU timing checkpoints appear in the native diagnostic file (`CHECKPOINT`) and
logcat (`PREVIEW_GPU_BREAKDOWN`). Keep all device GPU benchmark runs serial.

`HeadlessGpuTest#optimizedPreviewCompilerParity` compares an optional `glslc -O`
test shader to the reference with both highlight methods and synthetic lens
shading. `optimizedStridedBufferParity` additionally checks even padded and odd
RAW16 row strides against image input, with exact recovered RGBA16F bytes.
These diagnostic compiler variants do not change the production shader build.
