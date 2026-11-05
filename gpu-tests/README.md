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
./gradlew -Prawr.gpuTests=true :gpu-tests:assembleDebug :gpu-tests:assembleDebugAndroidTest
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

### RAW highlight guide comparison

`HeadlessGpuTest#guideDenseFieldParity` compares the preceding seed bound and
frozen smoothing shader with the per-channel bound, deferred clip-state read,
and nonempty-field smoothing specialization. The existing indirect controller
is enabled on every variant. The large scene runs reference → bound-only →
deferred-read → dense-smooth → reference, with 60 warmup/90 measured frames.
Exact recovered RGBA16F comparisons also cover clipped stress input, alternating
seeded/empty frames, no seeds, a one-cell guide and long one-cell-wide guides.
Dispatch arguments must show that the positive fixtures actually run smoothing.

The dense specialization is selected only for the optional preview controller.
Every jump-flood step is retained. From any seed, the signed binary steps of an
XY distance can reach every guide cell while remaining in bounds. Positive
weighted averages of the near-unit seed RGB sums cannot become empty. All 25
smoothing taps therefore contribute, with exact integer weight sum 256. Tap and
accumulation order, float32 arithmetic and RGBA16F pass boundaries are preserved.
The ordinary still/legacy pipeline keeps generic smoothing. The host
`guide_dense_reach_test` exercises coverage from every single seed in all small
rectangles, plus corner/center seeds in large, thin and non-power-of-two fields.

`--stage-timing` on the native preview benchmark adds four timestamps and prints
per-stage median/mean/p95/p99 for CFA, seed, propagation, smoothing and apply.
Without that option, the benchmark retains its two-query timing layout.
These fixed-input GPU times exclude the production scopes, look and display.

After the fixed comparison, `NativePreviewTest -e guideReference true` replaces
only that test engine's seed/smoothing modules with the immediately preceding
ones staged in `guide-dense-0-0`; its indirect controller stays enabled. Do not
combine this with `highlightReference`. Run reference/candidate/reference
serially, then clear `debug.rawr.vf_stage_timing` and repeat lifecycle checks on
the normal query layout. No app Activity is needed.

`HeadlessGpuTest#indirectGuideParity` checks the optional core-Vulkan indirect
guide controller against the preceding CFA shader and unconditional guide chain.
Six exact RGBA16F comparisons cover no clipping, clipped pixels without seeds,
valid bright seeds, alternating empty/seeded frames on all three frame slots,
unequal white balance, lens shading and partial workgroups. The test also checks
GPU-written dispatch arguments after fence completion, so a fixture that never
actually exercises recovery/propagation cannot silently pass.

The app enables this path by default. Select the legacy path before engine
creation with `adb shell setprop debug.rawr.guide_indirect 0`; clear the property
after comparisons.
It does not disable recovery: unclipped frames skip the guide/apply work and
clipped frames with no authoritative seeds skip only propagation/smoothing.
Seeded frames retain every original pass. There is no CPU predicate readback.
The implementation uses core `vkCmdDispatchIndirect`, with compute shader writes
made visible to `DRAW_INDIRECT` / `INDIRECT_COMMAND_READ`; it does not depend on
`VK_EXT_conditional_rendering`. The optional controller is not used by stills.
Synchronization follows the Vulkan specification's
[indirect dispatch requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDispatchIndirect.html)
and [dispatch chapter](https://docs.vulkan.org/spec/latest/chapters/dispatch.html).

For actual displayed pacing, run `app_preview_benchmark.py --surface-stats`
through `uv` in addition to native GPU timings. Read the SurfaceView layer's
`averageFPS` and `present2present` histogram in `surface-timestats.txt`.
Activity/UI frame counts and average GPU time do not establish stable 30 fps.
Run camera/UI and synthetic GPU measurements serially.

`HeadlessGpuTest#optimizedGuideCompilerParity` compares normal, propagation-only,
smoothing-only and combined `glslc -O` variants on a seeded large fixture. These
are diagnostic variants: exact output passed on Mali, but no meaningful speedup
was measured, so production compiler flags are unchanged.

`HeadlessGpuTest#highlightControlFlowParity` compares the preceding seed/apply
shaders with the current preview path, requiring exact recovered RGBA16F bytes.
It covers normal and highlight-stress input, unequal white balance, 17×13 lens
shading and partial workgroups. The optional `-e highlightScenes 1` selects only
the normal fixture for exploration; final validation uses the default three.
Reference shaders are staged in `files/gpu-results/seed-0` by this test.

After that test, `NativePreviewTest -e highlightReference true` loads those old
seed/apply modules into that test engine's private directory before camera
configuration. Without the flag it uses the current modules. The guide backend
reads literal workgroup sizes from SPIR-V, so old 16×16 and new 8×8 modules use
matching dispatch geometry. Run comparisons serially with identical arguments,
for example `-e frames 120 -e waveform true -e profile 0 -e detachSurface true
-e testUiPause true -e trace true`. This does not alter the main app's files.

Detailed GPU checkpoints can split CFA, highlight preparation/apply, guide seed,
propagation and smoothing. Enable `debug.rawr.vf_stage_timing=1` **before** engine
creation. Extra timestamp queries are disabled by default; inspect these stage
fields only when the checkpoint says `rawStageTiming=1`. Repeat lifecycle tests
with the property cleared to cover the normal query layout. Restore the empty
property and stop the headless package after the device session.
