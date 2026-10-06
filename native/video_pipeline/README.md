# Video RAW stage

`VideoDemosaic` owns the recording resolution RAW demosaic, crop or 2× Bayer
reduction, lens-shading sampling, and sensor clip-state output. It does not
read the lower resolution viewfinder image. Its output feeds the shared
post-demosaic processor in `native/post_demosaic`.

Layout: `include/video_pipeline/`, `src/`, `shaders/` (`video_demosaic.comp`
and `video_raw_core.glsl`, which the fused RAW-input tonemap variant also
includes). Android-only.

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
slightly higher detail, for ~2.5x the box path's GPU time (10.1 vs 4.0 ms at
the probe's idle clocks). `adb shell setprop debug.rawr.video_downscale box`
restores the box average for A/B; the journal's `rawStage` records which one a
recording used. The debug-only `VideoDownscaleProbeActivity` reruns the
zone-plate comparison on device without the camera; with `--ez compile_check
true` it instead reports `vkCreateComputePipelines` results for every `.spv` in
`files/spv_check`, to bisect driver compiler rejections. (Adreno rejects a
conditional store to the r16ui clip map in the strip pass, hence its
unconditional store.)
