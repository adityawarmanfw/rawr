# Still chroma cleanup integration

## Production behavior

- Standalone RCD and the RCD branch of RCD + VNG4 still capture compute the same
  robust frame balance on the GPU. Two reductions
  use 8x8 CFA blocks, rejecting any block with invalid/near-black/near-clipped
  samples (normalized .001–.987). Channel means produce relative gains in
  [.05,1]. Empty frames fall back to identity. RCD operates on balanced samples;
  export reverses these gains, preserving the camera-linear/WB contract.
- This follows the input-conditioning principle validated against RawTherapee,
  with normalized rejection thresholds appropriate to the app's input formats;
  it does not claim bit-exact RawTherapee AUTO preprocessing.
- The DNG editor computes the same frame estimate over 64-row CPU strips and
  reuses it for all standalone RCD 1024-pixel demosaic tiles. It retains bounded
  memory and avoids tile-dependent color estimates. Dual remains full-frame
  and uses the same GPU balance as capture. Demosaic/render cache keys are bumped.
- Still FCC uses absolute chroma, uniform averaging, and no display-range chroma
  bound. The factory setting remains two iterations. Video settings, VNG4 and
  the dual blend policy are unchanged. Production dual callers enable
  `autoBalance`; the library default remains false for frozen reference fixtures.
- Calibrated still processing uses optimized Lab defringe with the actual frame
  camera matrix. Strength zero bypasses it. Video retains the previous shader.
  Photo UI exposes strength; detection and bright-edge guards are automatic.
  Strength 1 applies the full *gated* correction and is the tested factory
  default, not a demonstrated optimum for all scenes. Legacy edge/luma fields
  remain parseable.
- Ordering remains demosaic → optional denoise → WB/highlight reconstruction
  → FCC → Lab defringe → SDR highlight compression → tone/film render.
  Highlight reconstruction is not replaced by a post-defringe clipping pass.

## Verification (2026-10-08)

- Android debug APK builds with JDK 17; native production components build for
  both macOS/MoltenVK and Android arm64 API 33.
- GPU RCD tests: all four Bayer patterns, native-sample and camera-linear scale
  preservation, partial workgroups, black/clipped frames, finite output/alpha.
- Dual follow-up: zero-contrast dual RCD matches standalone RCD bit for bit on
  all four Bayer patterns, including detailed, constant, black and clipped
  fixtures. Manual blends stay finite with exact alpha. Full-resolution auto
  dual + FCC2 + Lab replay of the original 16:08:24 frame also passes.
- CPU streaming balance test on phone: all patterns, partial blocks, clip/black
  rejection, empty fallback, and invalid fixed-gain rejection.
- Lab production-component tests: 1x1 and odd sizes, two calibrations, constant
  colors, bit-exact disabled mode, alpha, active repair with Y preservation,
  extreme HDR values, repeated resource reuse, and singular-matrix rejection.
- Replayed five full-resolution scenes: four RAWR captures at 17:23:26, 16:08:24,
  16:07:57, 16:07:42 and the clipped RAY 14:17:26 frame. Original calibration,
  WB, exposure and orientation retained. Purple edges remain possible; this is
  not a guarantee of artifact-free reconstruction or recovery of clipped detail.
- Production Lab on the Adreno 840 matches the optimized phone prototype within
  one 8-bit display code for every pixel of the original test image; alpha exact,
  all pixels finite. Lab scratch allocation: 25,460,032 bytes for 4080x3064.

Timing uses standalone production components and Vulkan timestamps, not camera
capture-to-JPEG latency. Each run uses two rounds, 20 warmups + 100 iterations,
reversed order on round two. The initial run measured RCD at 32.2–32.3 ms without
balance and 33.2–33.5 ms with balance; Lab took 11.7–11.9 ms. A later repeat slowed
both production and prototype: the stable second round measured RCD at 48.30 ms
without balance and 49.48 ms with balance, and Lab at 17.04 ms versus the
prototype's 17.07 ms. Thus balancing adds about 1–1.2 ms at these operating points;
absolute latency varies with phone conditions. No APK was installed and no phone
settings were changed.

The review images and detailed logs are retained locally under
`tmp/production_cleanup/`; they are intentionally not committed. Real-frame
comparison includes the expected effect of running highlight reconstruction,
which the earliest artifact-isolation renders bypassed.

## Reproduce tests

```sh
cmake -S tools/offline_rawr_pipeline -B tmp/offline_rawr_pipeline -G Ninja \
  -DPOST_DEMOSAIC_BUILD_TESTS=ON
cmake --build tmp/offline_rawr_pipeline --target rawr_offline_pipeline lab_defringe_validate
UV_CACHE_DIR=$PWD/tmp/uv-cache TMPDIR=$PWD/tmp uv run --no-project --with numpy \
  tools/offline_rawr_pipeline/validate_rcd_balance.py \
  tmp/offline_rawr_pipeline/rawr_offline_pipeline tmp/rcd_checks
UV_CACHE_DIR=$PWD/tmp/uv-cache TMPDIR=$PWD/tmp uv run --no-project --with numpy \
  native/post_demosaic/tests/validate_lab.py \
  tmp/offline_rawr_pipeline/post_demosaic/lab_defringe_validate tmp/lab_checks
```

`RCD_BUILD_TESTS=ON` exposes `rcd_balance_test` and `rcd_benchmark`; the latter's
`--auto-balance` flag includes GPU conditioning in its reported total.
`lab_defringe_validate INPUT OUTPUT WIDTH HEIGHT MATRIX.txt STRENGTH ITERATIONS`
benchmarks the actual production component with a row-major camera-to-sRGB matrix.
