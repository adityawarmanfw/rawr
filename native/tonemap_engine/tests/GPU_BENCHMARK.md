# Tonemap GPU benchmark

The Vulkan runner measures GPU dispatch timestamps on MoltenVK and Android.
Use `--bench-only` for timing or `--reference-shader` to compare against the
current shader before an optimization. The older CPU image oracle models
legacy controls; use GPU reference comparisons for the current renderer.

Compile matching workgroups and output formats for both shaders. Set
`RAWR_TONEMAP_COMBINED_CST=0` for the original gamut arithmetic, or `1` for
cached matrices. Texture variants require `RAWR_TONEMAP_NEUTRAL_TEXTURE=1`
and `--neutral-texture`. Experimental custom textures additionally require
`RAWR_TONEMAP_USER_TEXTURE=1` and `--user-texture`.

```sh
tonemap_vulkan_test --shader matrices.spv --reference-shader current.spv \
  --local-x 8 --local-y 8 --width 4080 --height 3064 \
  --warmup 20 --iterations 60
```

`--input-rgba16f FILE` accepts tightly packed camera-linear RGBA16F. Supply
its column-major transform with `--camera-matrix a,b,c,d,e,f,g,h,i` and
exposure with `--ae-post-gain` / `--exposure-ev`. Video shaders also require
`--video-monitor`, `--highlight-compression` (mapped coefficient, UI/100),
and `--highlight-gain`. These reproduce the unchanged video tone tap and
monitor write. They exclude demosaic, WB, camera ingestion and encoder work.

Use `--output-bits 32` with `RAWR_TONEMAP_OUTPUT_RGBA32F=1` for float parity;
`--output-bits 10` requires the RGB10A2 video shader. Float tolerance is
`1e-5 + 1e-5 * abs(reference)`; integer outputs allow one code value.
`--dump-output FILE` writes raw pixels; `--output FILE` writes RGBA8 as PPM.

Custom spaces use the numeric `Gamut` and `TransferFunction` enums with
`--lut-input-gamut`, `--lut-input-transfer`, `--lut-output-gamut`, and
`--lut-output-transfer`. Repeated `--lut FILE` arguments form a chain.
Placement, output action and intensity are selectable with `--lut-placement`,
`--lut-after-action`, and `--lut-intensity`.

Creation-time texture uploads need a caller-synchronized queue. There are no
per-frame uploads or submissions inside `record()`. The app enables Neutral
textures only for tested Adreno 840 video without a custom LUT. Custom chains
retain buffers. Non-unit domains, extended LUT values, and decoded log outputs
retain original custom CST arithmetic; sensitive post-render chains also
retain Neutral's original CST. Intensity zero bypasses LUT evaluation exactly.

## Tone control properties

`--tone B,S,C,M,H,W,SAT,VIB` sets the photographic controls (UI -100..+100;
requires `--bench-only`). `tests/tone_controls_gpu.py` drives the runner on a
synthetic AP1 chart and asserts the current renderer's invariants (finite,
monotonic gray ramps, Whites continuity at 0, stacking strength, LUT
overshoot, vibrance shadow guard, Blacks+ near-black slope). Pass
`--before OLD.comp` to print the same metrics for an older shader.

```sh
uv run --no-project --with numpy native/tonemap_engine/tests/tone_controls_gpu.py \
  --before tmp/tone-host/tonemap_before.comp
```
