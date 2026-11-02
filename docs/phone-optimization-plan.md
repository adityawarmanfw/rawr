# Phone performance optimization plan

## Scope and constraints

Target: RAWR, starting at commit `4c64cb1f5eb553bf61ca9a459c5a2c94964732e4`.
Connected device reports Xiaomi `25080RABDG`, device `lapis`, SoC `MT6878`.
Use this device's actual Vulkan capabilities, memory headroom, and thermal behavior;
do not assume an Adreno GPU or copy RawLens hardware tuning blindly.

Order: plan, measured baseline, optimization, paired verification.
Cover live RAW viewfinder, single-frame and enabled multiframe still processing,
highlight recovery, denoise, false-color cleanup, defringe, sharpening, color/tone,
JPEG encoding and durable publication. Implementation references authorized by
the user are RawLens (local), [PhotonCamera](https://github.com/eszdman/PhotonCamera),
and [MotionCam](https://github.com/tadit7/motioncam).

All work is destined for one MR, with a clean review branch based on `origin/main`
as required by `AGENTS.md`. Include this plan, baseline methodology and results,
focused implementation commits, quality validation, and before/after results.
Keep large generated benchmark artifacts out of Git; summarize results in the MR
and retain reproducible commands and source revisions.

Preserve resolution, demosaic selection, precision, highlight method, denoise and
cleanup settings, tone/color behavior, JPEG quality and chroma subsampling, HDR
mode, and metadata. Do not obtain speed by disabling features, reducing frames,
weakening recovery, lowering JPEG quality, or dropping durability guarantees.

### Vulkan portability and headless execution

Mali (including MediaTek devices) and Adreno remain required GPU families. Query
Vulkan features, limits, formats, memory types, subgroup properties and external
buffer support at runtime. Do not select an unsupported shader or infer features
from the SoC name. Keep the portable path; vendor-specific variants require a
capability gate, measured benefit, and narrowly scoped driver workarounds.
Do not assume fixed subgroup width, host-coherent memory, identical queue-family
ownership, or that multiple queues execute concurrently. Preserve Android camera
acquire/release fences and GPU resource lifetimes through completion. FP16 or
lossy/compressed intermediates are not permitted as an unmeasured quality shortcut.

Run analysis, compilation, CPU tests, stored-input comparisons, and benchmark
reporting headlessly. Actual camera/VF tests require the app in the foreground on
an awake unlocked device, driven by `scripts/ui`; no manual taps should be needed.
Record permission dialogs or locked-device limitations without entering credentials.
Treat unavailable GPU devices as untested, never as passing. Require physical
Mali and Adreno smoke/quality/performance results before declaring GPU portability
validated. Headless desktop Vulkan is supplemental evidence only.

Consult primary online sources throughout optimization, recording the relevant
advice and its limits. Initial sources (reviewed 2026-10-31):

* [Android Vulkan design guidelines](https://developer.android.com/ndk/guides/graphics/design-notes):
  explicit pipeline reuse, appropriate allocation/mapping and memory selection.
* [Arm Vulkan best practices](https://developer.arm.com/mobile-graphics-and-gaming/vulkan-api-best-practices-on-arm-gpus):
  GPU configuration/counters, synchronization and allocation/OOM investigation.
* [Qualcomm portable renderer guidance](https://www.qualcomm.com/developer/blog/2026/10/build-portable-rendering-systems-across-vulkan-directx12-with-qhwi):
  explicit optional capabilities, barriers and unified-memory considerations.
  Graphics/tile advice must not be applied to compute-only camera passes blindly.
* [MediaTek Dimensity 7300](https://www.mediatek.com/products/smartphones/mediatek-dimensity-7300):
  platform context for its Mali GPU; actual driver/features must still be queried.

GPU kernels, recovery and cleanup behavior remain unchanged until the relevant
baseline and correctness comparison can be run. CPU-side serialization/allocation
improvements can be evaluated independently with identical production inputs and
byte-level comparisons, but require device integration validation as well.

## 1. Establish reproducible baselines

Record source revision and dirty state, APK/build type, Android/driver versions,
Vulkan device and capabilities, camera/lens, sensor/output dimensions, complete
recipe, free storage, battery/charging state, and temperature. Confirm the
installed APK matches the source before attributing measurements to this commit.
Keep generated logs, traces, outputs, and reports under `tmp/phone-performance/`.
Use `scripts/ui` for device interaction and stop the camera after each run.

Use release-equivalent builds for performance comparisons. Use debug UI tooling
and diagnostic builds to investigate behavior, keeping their results separate.
Do not replace an installation during active rendering or remove application data.

Measure three warmups followed by at least 20 completed still captures per primary
case; report median, p95, range, and failures. Alternate baseline/candidate runs
where practical to expose thermal drift. Use fixed scene, camera, exposure, WB,
focus, and recipe. Separate first-use pipeline compilation from warm operation.
Measure sustained viewfinder operation for at least 60 seconds per primary case.

Primary cases:

| Case | Purpose |
| --- | --- |
| Current user recipe and camera | Real everyday baseline |
| Bright clipped scene, each available recovery method | Reconstruction and clipping behavior |
| High ISO, enabled denoise and cleanup | Spatial processing and memory pressure |
| Fine detail, saturated edges and gradients | Detail, color artifacts and banding |
| Largest supported still output | Peak allocation and export stress |
| Enabled HDR+/MFSR capture modes | Merge buffers and competing GPU workloads |
| RAW VF idle and VF during repeated captures | Frame pacing and contention |
| UltraHDR, if supported and used | Gain-map processing and encoding |

Record the following independently:

* RAW VF CPU/GPU frame time, delivered FPS, p95 frame interval, and dropped frames.
* Shutter request to frame acquisition, processing start, JPEG encode completion,
  write/fsync completion, and MediaStore publication. Also measure capture-ready
  latency, queue depth, and repeated-shot throughput.
* Per-stage demosaic, highlight, denoise, FCC, defringe/sharpen, color, tone/film,
  gain map, readback, encode, and synchronization/queue gaps.
* Process PSS/RSS and Java/native heap at idle, VF, capture peak, and post-capture;
  independently account for GPU allocations and buffer lifetimes where available.
  Do not add overlapping memory accounting categories together.
* CPU/GPU activity, thermal state and temperature, failures, and saved file size.

Existing stage timers and JPEG writer logs provide a starting point. The EXIF
processing total explicitly excludes file write and fsync; it is not the
shutter-to-saved metric. Correlate request IDs and monotonic timestamps through
publication. Add narrow measurement instrumentation only where necessary before
changing processing behavior. Sampling memory can miss short allocation peaks;
document that limit and use allocation/trace evidence for large transients.

## 2. Rank measured bottlenecks and inspect reference implementations

Trace the live preview scheduler and RAW buffer ownership, then
`SingleFrameCaptureJob`, the multiframe jobs, processing engines, and
`JpegCaptureWriter`. Compare equivalent buffer reuse, GPU submissions, readback,
encoding, and processing scheduling in RawLens, PhotonCamera, and the specific
MotionCam fork supplied by the user. Investigate their RAW VF, recovery, cleanup,
and multiframe implementations where present; do not assume feature equivalence.
Pin the inspected revision of every external reference. Explain each proposed
transfer in terms of RAWR's ownership model and the connected device's capabilities.
Check applicable file licenses and preserve attribution/notices for reused code;
record source paths and revisions in the MR. An external implementation is a
candidate approach, not evidence of a performance gain on this phone.

Prioritize measured end-to-end cost and peak live memory. Candidate investigations,
not established findings:

1. Remove redundant full-frame copies and overlapping lifetimes. Transfer ownership
   and release dead RAW/RGB/merge/staging buffers after their last GPU consumer.
2. Reuse bounded scratch allocations, pipelines and descriptor resources across
   captures. Give pools explicit limits and memory-pressure release behavior.
   Measure retained idle memory as well as peak memory.
3. Reduce unnecessary submissions, waits, readbacks and CPU/GPU round trips. Keep
   fences and barriers required for correctness; trace dependencies before fusing.
4. Optimize recovery and cleanup hotspots with equivalent arithmetic and scheduling.
   Preserve global statistics and neighborhood dependencies; naive tiling of
   recovery or wavelet processing can alter output or create seams.
5. Tune workgroups and dispatch geometry on this phone, preserving borders and
   odd-size behavior. Do not assume one workgroup fits all image sizes.
6. Inspect encoder input ownership, staging and writer scheduling for avoidable
   copies and stalls. Preserve quality, subsampling, metadata and successful fsync
   before reporting durable completion.
7. Bound concurrent capture/export/VF memory and contention. Confirm any scheduling
   change improves latency without silently rejecting accepted captures or causing
   unacceptable preview stalls.

## 3. Implement one measured improvement at a time

Start with the largest demonstrated copy, allocation, or synchronization cost.
Keep each change independently reviewable and reversible. For each change record:
baseline evidence, hypothesis, touched stages, ownership/synchronization proof,
before/after measurements, quality results, and limitations. Revert changes that
merely move cost elsewhere or exchange faster export for excessive idle memory.

## 4. Quality and correctness gates

Use identical stored RAW inputs and recipes for deterministic before/after image
comparisons; independently captured scenes cannot establish pixel equivalence.
Existing host replay is diagnostic and is not a complete app-equivalent pipeline,
so its results supplement production-path exports rather than replace them.

For copy/lifetime/scheduling changes, require identical decoded pixels and metadata
apart from intentional timing/provenance fields. Compare intermediate stage output
when necessary. Check dimensions, orientation, color metadata, subsampling, HDR gain
map and decode validity. For arithmetic/kernel changes, prefer exact parity; any
numerical differences require measured bounds and explicit review before acceptance.
PSNR/SSIM alone cannot establish unchanged highlight recovery or detail.

Inspect clipped-channel reconstruction, highlight hue, fine texture, shadow noise,
chroma edges, borders, gradients and tile boundaries. Exercise all CFA phases and
supported dimensions affected by a kernel change. Run relevant existing native and
Kotlin tests, then build/lint checks appropriate to the final change.

Verify repeated capture, rotation/backgrounding, cancellation, memory pressure,
writer errors and resource cleanup where affected. Confirm that memory returns to
the intended bounded steady state and saved files are complete and readable.

## Deliverables and acceptance

1. Reproducible baseline report and raw observations, with missing measurements
   marked explicitly.
2. Ranked bottlenecks and concrete implementation choices supported by traces.
3. Focused code changes with tests and deterministic quality comparisons.
4. Paired device results: absolute and percentage changes for shutter-to-published
   median/p95, peak memory, sustained VF pacing and repeated-shot throughput.

Do not promise a speedup percentage before measurement. Accept a change only when
its improvement exceeds run-to-run variation, preserves the quality gates, and
does not introduce a material regression elsewhere. Report any untested mode.
