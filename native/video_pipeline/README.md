# Video RAW stage

`VideoDemosaic` owns the recording resolution RAW demosaic, crop or 2× Bayer
reduction, lens-shading sampling, and sensor clip-state output. It does not
read the lower resolution viewfinder image. Its output feeds the shared
post-demosaic processor in `native/post_demosaic`.

Layout: `include/video_pipeline/`, `src/`, `shaders/` (`video_demosaic.comp`
and `video_raw_core.glsl`, which the fused RAW-input tonemap variant also
includes; `video_demosaic_quad.comp` and `video_quad_core.glsl` for 1:1
recordings). Android-only.

## 1:1 recordings (4K, Open Gate)

`video_demosaic_quad.comp` gives one thread a 2×2 Bayer cell. Crops are
even-aligned, so a pixel's channel is the same in every thread of a wave: no
per-pixel channel branching, and with the loops unrolled the roles fold to
constants. Green at red/blue sites is the same Hamilton-Adams estimator as
`video_mhc.glsl` (keep the two in sync). Red and blue are not MHC kernels: a
shared plane of packed half pairs holds `sqrt(G)` and, at red/blue sites,
`d = sqrt(sample) - sqrt(G)`; each missing value is `sqrt(G) + d` interpolated
from the nearest samples of that colour, weighted by
`1 / (0.03 + |sqrt(G) - sqrt(G_neighbour)|)`, so chroma follows green edges, and
squared back. The sample's own channel is kept exactly. The green plane's
edge guidance is what removes MHC's stripes along edges; plain unguided
colour-difference interpolation scored below MHC, and a chroma median lowered
every score, so neither is used.

Checked against ground truth built from device DNGs (2×2-binned to true colour,
re-mosaiced, so the truth is exact), RGGB and the other three CFA orders:
about +0.9 to +1.4 dB on real frames and +1.2 dB on a synthetic edge chart, where
visibly wrong-coloured pixels fall from 3.5% to 0.7%; roughly half as many at
ISO-like noise. Known weakness: a one-pixel line of a saturated colour on gray can
lose its colour. On the V2562 (Adreno) at 4080×3064 with lens shading the stage
takes 5.9 ms against 7.7 ms for the tiled MHC build. The clip-state image is
identical to the tiled build's. Pipeline creation falls back to the tiled
`video_demosaic_1x` build if a driver rejects the quad shader; the journal's
`rawStage` reports `quad_guided_chroma` or `tiled_mhc_5x5`. 2× reduction keeps
the tiled and strip passes below. A speed-only variant with MHC maths (same output
as the tiled build, 3.3 ms) and the pattern as a specialization constant (about
4% more) were measured but are not used.

`VideoProcessingConfig` is the native settings contract. Highlight method,
threshold, compression, lens shading, and profiled denoise can update while
recording. FCC capacity and defringe pipeline parameters are latched when a
recording starts; status reports when a settings change needs a new recording.
The Video false color correction switch maps FCC steps to zero for video only;
still capture keeps its configured 1–8 steps. Changing this switch takes effect
on the next recording because the FCC pipeline is allocated at start.

The recorder's timestamp policy is selected at recording start through
`NativeVideoRecorder.Settings.timestampPolicy`. Kotlin can enumerate
`VideoTimestampPolicy.entries` and query
`NativeVideoRecorder.supportedTimestampPolicies()` before showing a choice.
`REALTIME` is supported: each encoder frame carries its RAW sensor timestamp
(converted to `CLOCK_MONOTONIC`) through `VK_GOOGLE_display_timing`, so a late
present keeps its capture time instead of leaving a gap. Drivers without the
extension fall back to the queue time. `FIXED_CADENCE_REPEAT` is reserved but
unavailable until the native path can present a repeated frame for each missing
cadence slot. Rewriting muxer timestamps without those frames would change
playback speed relative to audio.

Recordings at half the sensor crop (1080p from the 3840x2160 crop) use two
separable passes instead of a 2x2 box average. `video_downscale_h.comp`
demosaics wide 4-row strips (shared MHC math in `video_mhc.glsl`, ~10% apron
overhead) and filters them horizontally; `video_downscale_v.comp` filters
vertically. Both work in white-balanced luma/chroma: luma with a Lanczos-3 cut
at 0.87x the output Nyquist plus an anti-ringing clamp, chroma at half that
bandwidth to match the encoder's 4:2:0 storage. On a Bayer zone plate this cuts
aliasing (moire, stair-stepped edges) about 5x and false colour about 2x at
slightly higher detail, for ~2.1x the box path's GPU time (6.1 vs 2.9 ms in
the probe). About 3/4 of it is the strip pass, mostly the full-resolution
demosaic that supersampling needs; intermediate-format and strip-shape
changes measured no gain. `adb shell setprop debug.rawr.video_downscale box`
restores the box average for A/B; the journal's `rawStage` records which one a
recording used. The debug-only `VideoDownscaleProbeActivity` reruns the
zone-plate comparison on device without the camera; with `--ez compile_check
true` it instead reports `vkCreateComputePipelines` results for every `.spv` in
`files/spv_check`, to bisect driver compiler rejections. (Adreno rejects a
conditional store to the r16ui clip map in the strip pass, hence its
unconditional store.)
