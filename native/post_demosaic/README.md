# Post-demosaic processing

`rawr::post::PostDemosaicProcessor` owns the GPU stages after demosaic:
optional profiled denoise (`raw_denoise`), white balance, either highlight
method (`raw_highlight`), false-colour correction, optional defringe, and the
SDR Inpaint Opposed tone tap. Still capture, video and the DNG renderer use
separate instances of this implementation. The app owns session, frame and
encoder resources; Kotlin passes processing settings and receives status.

Layout: `include/post_demosaic/`, `src/`, and `shaders/` for the stages
this package owns (white balance, defringe). `CMakeLists.txt` also embeds the
raw_highlight, raw_denoise and false-colour SPIR-V it dispatches.

## Still chroma cleanup

Calibrated still callers pass the frame's post-WB camera-to-linear-sRGB matrix
as the final `record()` argument. This selects `LabDefringe`, after highlight
reconstruction and FCC, before the SDR compression tap and tone rendering.
The three passes compute packed half scores, reduce their frame mean, and
replace neighborhood Lab a/b while retaining center L. Defaults: strength 1,
threshold 13, blue-purple hue selection, and a .10–.25 XYZ-Y bright-edge guard.
The matrix and inverse travel in push constants; no scene-specific constants
or CPU image readback are used. Scores consume two bytes/pixel plus group sums.
Strength zero bypasses the stage without allocating its resources. Video calls
without calibration retain the existing inexpensive defringe and controls.

Photo settings expose strength; old edge-threshold/luma-floor recipe fields
remain readable for compatibility but apply only to the legacy video method.
Do not apply a late highlight safety clamp to un-reconstructed RGB and expect
this stage to repair all partially clipped edge colors: reconstruction belongs
before FCC/defringe, with the original sensor/WB/shading information.

Build the production-component file validator with
`-DPOST_DEMOSAIC_BUILD_TESTS=ON`. Run:

```sh
UV_CACHE_DIR=$PWD/tmp/uv-cache TMPDIR=$PWD/tmp uv run --no-project --with numpy \
  native/post_demosaic/tests/validate_lab.py \
  tmp/offline_rawr_pipeline/post_demosaic/lab_defringe_validate tmp/lab_checks
```
