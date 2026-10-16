# Raw denoise (darktable denoiseprofile wavelets)

GPU port of darktable's `denoiseprofile` wavelets path: Y0U0V0 colour split,
variance-stabilizing transform driven by the Camera2 `SENSOR_NOISE_PROFILE`,
à-trous wavelet thresholding. It runs after demosaic on pre-WB camera-linear
RGBA16F, for single-frame stills, video, and the DNG renderer
(`PostDemosaicProcessor`, `RendererEngine`). Multiframe stills skip it.

## Pipeline

`DenoisePipeline` tiles the image (1536 px tiles plus a halo of
`2*(2^scales-1)` px). Per tile: `denoise_img_precondition` (VST + Y0U0V0),
then per scale `denoise_img_atrous` (estimate), `denoise_img_threshold`
(on-GPU reduction), `denoise_img_atrous` (apply), and finally
`denoise_img_backtransform`. Thresholds are per tile, as in darktable's tiled
mode, so `record()` never waits on the host. Profile math (VST exponent,
WB-adaptive matrices) is float32 on the host, in the oracle's stage order.

## Validation

`tools/denoiseprofile_oracle.py` is a float32-pinned NumPy/Numba port of the
darktable source; `tools/freeze_fixtures.py` froze the goldens in
`tests/data/`. CMake builds `denoise_bench`;
`tools/verify_denoise_parity.py` runs the
production pipeline on every fixture and checks it against the goldens (see
the tolerances in that script) and checks that a four-tile run has no seams.

`denoise_bench` also reports GPU time on device. The `sweep_*.py` and
`experiment_v3.py` scripts are the oracle-side tuning studies behind the
shipped defaults.
