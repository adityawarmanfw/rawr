# Phone optimization results: first measured change

## Foreground validation and Home lag investigation

User authorized Activity/UI testing. Actual UltraHDR captures with the opt-in
`debug.rawr.single_primary_queue=1` completed while preview remained foreground:
13 successful captures including one warmup, 12 measured median12875.67ms,
p95/max15759.91ms, min11499.04ms. The initial secondary-queue run published after
55848.54ms and a Home transition; this is not a controlled A/B. Scene/exposure
changed. UltraHDR is distinct from excluded HDR+ multiframe processing and stayed
enabled. Primary queue selection remains opt-in; no Adreno validation.

Three actual Activity Home/resume cycles passed with the same process and camera
closed: foreground PSS570592–603252KiB, Home PSS120634–126221KiB. A subsequent
capture/post-publication Home check retained122497KiB. Shutter immediately
followed by swipe Home also published successfully in7213.51ms after the atomic
UI-presentation gate was introduced. These tests do not prove animation smoothness.

User reports Home improved but still laggy. Launcher gfx counters were noisy;
neither a two-frame preview backlog nor a750ms resource-reclamation delay showed
a clear win. Both experiments were removed, settings restored to three frames
and zero delay. No rejected experiment is enabled in production.

A12-second Perfetto frame-timeline trace captured the issue: around the Home
gesture, launcher RenderThread blocked199.21ms in dequeueBuffer, RAWR presentation
waited119.20ms and GPU completion waited122.19ms. Window focus changed at trace
4.775s; Activity pause was6.688s. RAWR continued presenting throughout that gap.
This supports gating preview on focus loss earlier than Activity pause. It does
not isolate all GPU cost or prove a complete fix. Trace and query artifacts are
under ignored `tmp/phone-performance/perfetto/`.

The focus-loss APK then showed its last QueuePresentKHR starting at6.714s,
focus change6.727s, Activity pause7.331s, with no later presentation calls.
The launcher max dequeueBuffer wait in that trace was70.40ms versus199.21ms
in the preceding trace. This is a single trace pair with different timing and
scene, not a percentage-performance claim or proof of completely smooth Home.
CameraService showed no active client afterward; Home PSS97709KiB. The final
APK contains neither the rejected backlog limit nor resource-release delay.
Final focus-gated Activity validation passed three swipe-Home/resume cycles,
with Home PSS111075–111445KiB and camera closed in every cycle. Gesture-window
framestats contained only1–3 eligible frames per cycle; zero missed deadlines
there is insufficient coverage to certify smoothness. Final shutter-then-Home
test published successfully in6061.67ms with the primary-queue experiment on,
and closed the camera afterward. The benchmark APK was left installed and the
app stopped. The experimental queue property is restored off after testing.

Native telemetry still reports roughly70.7ms per processed VF frame, including
51.1ms RAW and9.4ms scopes, with ingress drops. The UI's sensor FPS readout does
not establish presentation FPS. Whole-app optimization remains incomplete.
Frame-timeline interpretation follows [Perfetto documentation](https://perfetto.dev/docs/data-sources/frametimeline).

## Single-frame optimization continuation

Scope now excludes HDR+ and MFSR. No resolution, image arithmetic, quality,
chroma subsampling, cleanup setting or durability guarantee was reduced.

Implemented in separate commits:

* Renderer tile/waveform readback prefers compatible cached host memory, with
  coherent fallback, transfer-to-host dependency, fence and noncoherent
  invalidation. Upload allocations keep their coherent write path.
* Direct preview output profiles do not allocate the separate Inpaint Opposed
  tone image until needed. Three slots at the native 4080×3064 benchmark size
  avoid 71.5 MiB of logical RGBA16F storage before alignment. This is an image
  allocation estimate, not measured resident PSS. Switching to a profile that
  needs the image allocates it and retains it safely thereafter.
* OFF/opposed post-demosaic processing defers colour-propagation guide images
  and recovery/guide pipelines until that method is actually used.
* Single-frame surface detachment releases preview engines, images, scopes and
  import caches after camera retirement and GPU completion. Active capture
  work keeps the existing deferred-detach policy; enabled multiframe bypasses
  this new release. Capture workers and the Vulkan device/cache are preserved.

### Fixed-input pixel checks and replay measurements

Cold replay uses a copied captured 4080×3060 DNG and an empty linear cache for
each sample, RCD, FCC2, defringe, JPEG98 4:2:2. Full decoded ARGB SHA-256 and JPEG
sizes match across APKs for RawrBase/opposed (three samples), sRGB/propagation
(two) and AgX/opposed (one). These sample counts do not establish all-scene
quality or every cleanup/profile combination. They do verify exact decoded
output for the fixed cases; no production shader arithmetic changed.

| Cold replay stage | Before median | After median | Samples per APK |
| --- | ---: | ---: | ---: |
| RawrBase/opposed, cached readback only | 6199 ms | 6138 ms | 3 |
| RawrBase/opposed, including lazy guide resources | 6199 ms | 5889 ms | 3 |
| sRGB/propagation, lazy guides vs eager guides | 5831 ms | 5787 ms | 2 |
| AgX/opposed, lazy guides vs eager guides | 5886 ms | 5766 ms | 1 |

Timing includes open/render/fsync/close, excludes fixture copying and decoded
hash calculation, and ran under competing foreground workloads. Small noisy
samples are exploratory; do not present them as a proven percentage improvement
or capture-to-gallery timing. Compare reports with
`uv run --no-project python tools/phone-performance/compare_replay.py BEFORE AFTER`.

The optimized headless native GPU suite passed **7/7 in 74.611 s**, including
exact recovered linear-byte parity across bidirectional preview tone switches
on all three slots. JVM tests and the actual benchmark app APK build passed.

### Production native preview lifecycle, no Activity

The new headless test drives RAWR's actual Camera2/Vulkan engine into a
SurfaceTexture drained by an offscreen EGL consumer. It uses CAMERA permission
and a camera foreground service, the built-in camera profile and shipped
zero-copy setting. It does not open the app UI. The initial generic/default-native
setup failed every frame with `bridge copy requested without owned image`;
the app's shipped zero-copy setting succeeds. This exposed the legacy-copy
allocation defect for separate correction. ImageReader and EGL attempts alone
did not fix that configuration difference.

Measured stopped PSS after surface detach and a one-second drain:

| Native lifecycle cycle | Before preview release | After preview release |
| --- | ---: | ---: |
| 0 | 644702 KiB (629.6 MiB) | 113107 KiB (110.5 MiB) |
| 1 | 694943 KiB (678.7 MiB) | 112137 KiB (109.5 MiB) |

Both cycles resumed, with zero late frames during the subsequent one-second
observation. Six sRGB cycles and two cycles each RawrBase, Rec709 and AgX also
passed. Background CPU before the idle-poll follow-up was about 5–9 ms per
one-second observation. PSS includes the test/EGL consumer; this is **not** an
Activity/Home-gesture benchmark, a power measurement, or validation of capture
or recording during background transitions. Keep those acceptance gates open.

Final follow-ups passed: the corrected legacy-copy path presented 30+ buffers
in both start/stop/resume cycles (previously zero), and a six-cycle zero-copy
stress run retained 109921–112921 KiB after detachment, with zero late frames.
Empty ingress polling now slows from 10 ms to 100 ms only after idle teardown;
real events, lifecycle changes and shutdown wake immediately. Surface attach
restores normal polling before activation. Observed idle process CPU was 3–5 ms
per one-second interval (includes harness/EGL overhead, not a direct power
measurement). The legacy path intentionally retains its required R16 image;
the normal zero-copy path keeps its prior allocation policy. Final benchmark
APK build and JVM suite passed.

Reports remain under ignored `tmp/phone-performance/`; production preview
reports are in the test package's `files/gpu-results/native-preview-*` folders.
See [headless commands and limitations](../gpu-tests/README.md). Physical Adreno,
film/LUT/denoise coverage, live UI pacing and full single-frame capture-to-gallery
validation remain pending. MR publication still requires GitHub authentication.

## Headless APK continuation, 2026-11-01

Added `gpu-tests`, an Android instrumentation target with no Activity/launcher.
It embeds existing native GPU tests and stages the actual production benchmark
renderer JNI library and assets. See [commands and coverage](../gpu-tests/README.md).
The user explicitly authorized headless camera use/permission; opening the app
UI is still not authorized.

Built and installed target/test APKs on the connected Mali-G615 phone. The real
camera test passed: three public RAW_SENSOR 4080×3060 acquisitions, matching
metadata timestamps, fsynced DNG files, production RCD/opposed/FCC2/defringe
JPEG98 4:2:2 replay, full-size JPEG bounds and sampled decode. The provider test
passed pending -> copy/fsync -> publish -> size/decode -> delete-own-row checks.
This run took 9.367 s overall; production replay was 6977.32 ms for a 12,703,392
byte JPEG, followed by 273.06 ms for provider copy/publication. These separate
stages are **not** production capture-coordinator end-to-end measurements or
a controlled optimization A/B. Scene was dark, ISO12923, exposure30.0ms.

Afterward CameraService reported no active camera clients; Stremio remained the
foreground Activity. No app UI was opened. DNG/JPEG fixtures and raw reports live
under ignored `tmp/phone-performance/headless-apk-results/`.

The initial tone CPU parity assertion failed. The existing runner documentation
states that oracle models legacy controls; it cannot establish current renderer
parity. The harness now compares current GPU output with the original CST shader
arithmetic, with matching 16×8 compile/dispatch geometry. No production shader or
quality tolerance was changed to hide the obsolete oracle failure.

The final GPU instrumentation suite passed all six tests in 80.962 s, including
the original-CST comparison, Bayer/CFA checks and exact cached readback.
GPU timings ran alongside the user's foreground workload and while charging;
they do not establish idle power or an isolated performance baseline. Initial
full-resolution preview medians were about 49.95 ms for method0 and 29.36 ms for
method1; inspect native timing targets separately from JUnit results. All-profile
quality/stability, live VF pacing, production background lifecycle and physical
Adreno validation remain pending. Home-swipe lag and background memory/power are
now explicit priority gates in the plan.

Status: local draft MR branch; native JPEG memory improvement implemented and tested. See
the continuation below for successful APK builds, app tests, and subsequent findings. The full
RAW-VF-to-published-JPEG optimization plan is **not complete**. APK/lifecycle,
shutter-to-publication, denoise/cleanup tuning, multiframe integration and physical
Adreno validation remain outstanding. Do not treat the native results below as
end-to-end capture results or proof of cross-GPU sign-off.

## Baseline and method

Baseline source: `4c64cb1f5eb553bf61ca9a459c5a2c94964732e4` (`origin/main`, confirmed
with `git ls-remote` on 2026-10-31). The phone has RAWR 0.5.1/versionCode 28
installed; its exact binary provenance has not been established, and it was not
replaced. Native executables were built from the identified baseline/candidate
source instead of attributing installed-app behavior to a source revision.

Phone: Xiaomi `25080RABDG`, `lapis`, MT6878, Android 16, Mali-G615 MC2. Vulkan
reports driver `v1.r44p1-01eac0.ed1fb6cfc1040479b92ddf50a952e57c`, subgroup size 16,
max compute workgroup invocations 1024 and one queue family with two queues.
Feature availability must still be queried at runtime; these observations do not
establish the same behavior on another Mali or Adreno driver.

Native CPU binaries used the installed Android NDK 27.0.12077973, API 33, static
C++ runtime, `-O2`, and the same bundled libjpeg-turbo 3.2.0 Release build with
ARM64 SIMD. Both variants use identical deterministic RGBA8 input, accurate DCT,
quality 98, 4:2:2 and eight encoder threads. These native-only tests are separate
from the app's pinned NDK 29 build requirement.

Each dimension ran baseline/candidate/candidate/baseline, three warmups plus 20
measured samples per process: 40 samples per variant per dimension. Each process
wrote one additional untimed JPEG. Individual samples and thermal snapshots are
retained under `tmp/phone-performance/device-results-final/`. The phone was
charging; reported thermal status stayed 0. CPU/load/frequency variation was not
controlled, so observed timing gains need app-level confirmation.

## JPEG memory and timing

The old parallel encoder allocated a contiguous output JPEG while all compressed
strips were still live. The standard JPEG writer now consumes a small combined
header and the owned compressed strips directly, avoiding that allocation and
full-image copy. Restart markers reuse dead strip-header bytes so writes remain
one header plus one large chunk per strip. The single-thread fallback and
contiguous API needed for UltraHDR are retained. Buffers are freed before fsync;
write errors stop output and never trigger another encode into a partial file.

The following times cover encoding and writes to `/dev/null`, **not** real storage
durability, MediaStore, shutter latency, or UI. RSS is process peak RSS from
`getrusage`, including the input and codec, not full RAWR memory or GPU memory.
The median/p95 below pool the 40 samples; p95 uses nearest rank.

| Input | Variant | Samples | Median ms | p95 ms | Peak RSS MiB, range across processes |
| --- | --- | ---: | ---: | ---: | ---: |
| 4080 × 3064 | Baseline | 40 | 67.70 | 92.35 | 90.94–93.41 |
| 4080 × 3064 | Segmented writer | 40 | 54.27 | 57.73 | 79.83–80.97 |
| 8160 × 6128 | Baseline | 40 | 247.05 | 295.61 | 314.95–315.05 |
| 8160 × 6128 | Segmented writer | 40 | 208.89 | 260.93 | 262.78–263.31 |

The 50MP case eliminates a 54,892,491-byte contiguous buffer (52.35 MiB), matching
the roughly 52 MiB observed RSS reduction. The 12.5MP JPEG is 13,722,300 bytes.
A separate initial ABBA run also reduced RSS, but showed much greater timing drift;
it is retained under `tmp/phone-performance/device-results/`. Memory improvement
is the robust finding; the table is not a promise of faster shutter-to-saved time.

Saved baseline/candidate JPEGs were byte-identical on the phone and on host:

* 12.5MP SHA-256: `eed5ae0d772f1c8cb582a0c4334709c51032cad7250745da324a45d4816610cc`
* 50MP SHA-256: `de7fd91787483f2a94a4fa3dbe8d9f5f8b76a7b5da5401f849cfe9308dce9dce`

No demosaic, highlight, denoise, cleanup, color/tone, precision, resolution,
subsampling or quantization setting was changed. No Vulkan shader, dispatch or
synchronization behavior was changed. Physical Adreno testing remains required;
CPU-only implementation scope reduces that risk but is not a hardware test.

## Sustained Mali RAW preview compute baseline

Unchanged production preview code ran headlessly at 4080 × 3064 input with RGB and
CFA-state output, three frames in flight, 60 warmup and 1,800 measured frames per
case, paced toward 30 FPS. Each case ran at least 60 seconds. These are GPU
timestamp durations, not camera acquisition, display, or full-frame still timings.

| Existing recovery method | Fixture | Median GPU ms | p95 GPU ms |
| --- | --- | ---: | ---: |
| Colour propagation (0) | Normal | 39.96 | 47.66 |
| Colour propagation (0) | Highlight stress | 77.49 | 79.29 |
| Inpaint Opposed (1) | Normal | 25.21 | 27.06 |
| Inpaint Opposed (1) | Highlight stress | 27.70 | 29.16 |

All runs passed the three-frames-in-flight check. The benchmark prints timing
`FAIL` because its hardcoded median target is 1 ms, which none of these phone
cases meets; that is a performance threshold failure, not a correctness pass.
Colour propagation also exceeds the 33.33 ms budget for 30 FPS in this fixture.
This is evidence to profile its guide passes and bandwidth next. Switching methods
would alter image processing and is not an accepted optimization in this MR.
Raw GPU results are in `tmp/phone-performance/device-vulkan-results/`.

## Validation

* Host JPEG suite: 2/2 passed. Sixty synthetic recipe/dimension combinations,
  including small-image fallbacks, cover quality 95/98/100, 444/422/420/gray,
  1080p, odd/aligned 12.5MP and 50MP. Decoded pixels match one-thread TurboJPEG;
  streamed bytes match contiguous output. Every failed sink position stops output.
  The same 60-combination suite also passed natively through ADB on the phone
  with zero failures (`tmp/phone-performance/device-jpeg-strip-check.txt`).
* ASan/UBSan JPEG suite: 2/2 passed (162 seconds). The codec library itself was a
  Release build; sanitizer coverage is for the new wrapper/writer/test code.
* Production asynchronous writer, native execution on the phone: passed metadata
  insertion/entropy equivalence, small-image fallback, reuse, rejected destinations,
  real write failure, oversized metadata, reset/join, and UltraHDR mux checks.
  It uses real Android log/property APIs, real file IO, fsync and close.
* Mali RAW preview RGB reference comparisons (reconstruction disabled in the
  existing CPU-reference harness): 24 cases passed, all four Bayer
  patterns and four geometries; maximum error below 0.000483 relative to the CPU
  reference. This is the existing preview's half-float tolerance, not an arithmetic
  change in this MR.
* Mali CFA-state checks: four Bayer patterns passed, zero mismatches across
  3,125,280 output cells per pattern at 4080 × 3064 input.
* Broader existing non-Vulkan host suite: 44/45 passed. `camera_profile_test`
  fails at the existing Vivo F35/F85 `captureBurst == 3` assertion. The same
  failure was independently reproduced from a detached, unchanged `4c64cb1`
  worktree. No camera profiles or their parsing/tests changed in this MR.
* Full Gradle APK/unit-test invocation could not start: Gradle 8.11.1 download
  fails resolving `services.gradle.org` in this session. Cached Gradle/AGP/SDK/NDK
  versions also differ from the app's pinned versions. No toolchain requirements
  were weakened to manufacture a passing build. Release lint remains unrun.

## Reference review

No external implementation was copied in this change. The inspected references
inform the remaining work, rather than establish an Android performance result:

* RawLens at `54b5d1db542b980a9ba6e80031d729ff82588c0c`,
  `app/src/main/cpp/native_jpeg_encoder.cpp` (clean at review): direct file destination and
  larger output buffering for FUSE. This change likewise avoids an unnecessary
  assembled-image copy while retaining RAWR's existing parallel strip encoding.
* [PhotonCamera](https://github.com/eszdman/PhotonCamera/tree/b2eff821a858033515eb2700e7292befecc95225),
  `GLTexture.java`: bounded/typed readback, asynchronous PBO/fence lifetime, and
  explicit cleanup. Its OpenGL synchronization cannot be transplanted into Vulkan.
* [MotionCam fork](https://github.com/tadit7/motioncam/tree/ea628e5b9bacd74c3f423aa92c259acf6d916479),
  `libMotionCam/libMotionCam/source/ImageProcessor.cpp`: native buffer lock ownership
  via `NativeBufferContext` and Halide processing buffers. Again this is a design
  reference, not equivalent RAWR/Vulkan code. Both external repositories carry GPL-3.0
  license text; any future copied files need their own license/notice review.

Primary Android/Arm/Qualcomm/MediaTek research links and portability constraints
are recorded in [the plan](phone-optimization-plan.md). Reproduction commands and
the result validator are in [the tooling README](../tools/phone-performance/README.md).

## Required before declaring the full plan complete

1. Build the app with its pinned toolchain; install the review build without losing
   the user's app data or interrupting a render, then measure real shutter-to-
   published-file and capture-ready latency with request-correlated monotonic clocks.
2. Trace complete still/multiframe processing, allocation lifetimes and camera/VF
   contention. Use the native preview baseline to guide further investigation;
   optimize recovery/cleanup only after equivalent-output comparisons are runnable.
3. Run camera/lifecycle and sustained UI frame-pacing checks using `scripts/ui`.
4. Repeat affected-path smoke/quality/performance checks on physical Adreno hardware.
5. Publish the MR branch and attach these results; leave it draft until the missing
   build and device checks are resolved.

Branch publication was attempted with `git push -u origin perf/phone-capture-pipeline`
and failed resolving `github.com` in this session. The branch is local; no remote
MR has been created. A review description is prepared at
`tmp/phone-performance/mr-body.md`.

## App and whole-viewfinder continuation, 2026-10-31

Network downloads now work. Installed pinned Gradle 8.11.1, SDK/build-tools 36,
NDK 29.0.14206865, CMake 3.31.6 and a local JDK 17. `assembleBenchmark` builds
successfully with native optimization and R8; all 436 debug JVM tests pass.
The signed benchmark APK installs alongside the original app. It was opened and
tested via `scripts/ui`; the original release installation/data were not replaced.

The user's current VF resolution is **Fast**. This setting lowers the look-stage
resolution, while RAW preparation and some monitoring still run at the larger
preview dimensions. It does not guarantee reduced RAW shader cost. The optimization
scope includes the complete VF, every render profile and film pipeline, overlays,
scopes, presentation, capture contention and memory. No claim is made that every
profile combination has been tested or optimized yet.

The original lower-priority still queue left the first AMaZE/UltraHDR capture
pending while preview was running. Sending Home allowed it to complete, taking
110,426 ms in total. This is a diagnostic observation, **not a normal baseline**.
An equal local-priority Mali experiment completed four foreground captures, then a
longer run completed 22 publications before `VK_ERROR_DEVICE_LOST` and an uncaught
slot-fence exception on the 23rd capture. The cause is not established. This is a
failed stability run, so the scheduling experiment is now opt-in only through
`debug.rawr.mali_fair_queue=1`; production and Adreno priority defaults are preserved.

Cached readback is a verified independent bottleneck: this Mali exposes coherent,
uncached memory type 0 and cached, noncoherent type 1. The headless GPU-fill/readback
test verifies exact bytes with both types. CPU reads of 16 MiB took 134.5–150.0 ms
uncached and 8.66–8.69 ms cached. Production readback now prefers compatible cached
memory, records TRANSFER_WRITE→HOST_READ, waits for the final render fence, then
invalidates noncoherent full-allocation ranges. Coherent memory is the fallback.
No added full-image CPU copy or change to profile arithmetic is involved.

Preliminary app HDR fit times fell from roughly 5–8 seconds in the preceding
foreground runs to 466–770 ms in four cached-readback captures. The last three
enqueue-to-publication times were 11,083, 6,843 and 6,290 ms. The final UI snapshot
failed after all four publications, so the runner did not produce a completed
summary. Scene/exposure changed between runs. These are exploratory observations,
not a controlled before/after claim. Retain `app-hdr-before` and `app-cached-smoke`
under `tmp/phone-performance/` for the raw data.

Two headless VF experiments were rejected: 16×8 groups and hardcoded Bayer scale 1.
Neither improved the median reliably over the 8×8 baseline (about 25.9 ms) and both
had worse p95 in their initial runs. They remain untracked diagnostic artifacts,
not production shader changes. No quality reduction was accepted.

The next candidate removes a live CPU RAW lock/sample used only for logging from
normal preview; it remains available with `debug.rawr.raw_content_stats=1`. Another
candidate releases completed processing GPU scratch after final readback, ahead
of JPEG encoding, retaining mapped output and explicitly persistent editor engines.
These compile, but live pacing/memory and profile stress validation remain pending.

The user has now requested **no app opening without waiting for them**. The app
was stopped with `scripts/ui stop`; the new 25-capture stability run was not
completed. Continue headless tests only, then obtain go-ahead before camera/VF UI,
profile, lifecycle or camera-to-display latency testing.

MR push now reaches GitHub but fails authentication (`Invalid username or token`);
`gh auth status` also reports no login. The branch and granular commits remain
local. No merge request has been created. Do not merge until the remaining
stability, profile quality, frame pacing and physical Adreno checks are complete.
