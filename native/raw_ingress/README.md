# raw_ingress

GPU ingress for camera RAW formats the realtime pipeline can't consume
directly.

- `Raw10Unpacker` (`shaders/raw10_unpack.comp`): reads MIPI RAW10 5-byte
  groups at `y * rowStrideBytes + x / 4 * 5` from the camera buffer, imported
  as a storage buffer, and writes four `r16ui` texels. The app records it into
  its owned R16 slot image in place of the CPU lock/unpack/upload
  (`pipeline/RawCpuUploadPool`), which stays as the fallback.

- `RawRemosaicker` (`shaders/raw_remosaic.comp`): for lenses whose RAW holds
  same-colour blocks (quad 2x2 or 4x4) while the camera reports Bayer, reads
  the camera buffer (RAW16 rows or RAW10) and writes Bayer in the block
  order to the same owned R16 slot image, so the viewfinder, video and the
  multiframe ring all see Bayer. Three passes: green everywhere (gradient-
  weighted from the neighbouring green blocks), per-block colour difference,
  Bayer out. Mirrors the CPU reference `imaging/CfaRemosaic.h` that stills use;
  `tests/native/raw_remosaic_vulkan_test.cpp` compares the two.

The host test `tests/native/raw10_unpack_vulkan_test.cpp` checks the shader
against the app's CPU `unpackRaw10Row`, including padded strides.

## Hexadeca reduction

`RawRemosaicker::Frame::hexadecaToQuad = true`, `block = 4`, and
`quality = High` or `Detail` reduce a native Hexadeca input to half width and height.
The destination must be an R16_UINT image of those reduced dimensions.
The three dispatches are exact same-color 2x2 sums, directional green plus
Quad color statistics, and High Bayer reconstruction. Sums and output retain
quarter-DN units: black/white calibration must be multiplied by four when
interpreting stored codes. Input must be <=16383; dimensions must be multiples
of eight. RAW16 and packed RAW10 with padded rows are supported.

High without reduction supports block=2 at the input dimensions, in whole-DN
units. High block=4 without reduction is rejected; it has not been ported.
Default Fast/Standard calls retain their original behavior. `Detail` (quad only: block=2, or the Hexadeca reduction)
uses cubic green and per-pixel colour differences from wanted-colour pixels within 3 pixels;
about 1.3 dB better than High against ground truth with the same real-sensor grid behaviour
(tools/hexadeca_reference/README.md), at ~25 ms for the output pass on Adreno 840 (tiled through shared memory; High is ~38 ms).
`Frame::phaseFit` (Hexadeca reduction only) first fits the sensor's per-position
response from the frame's own flat 4x4 blocks and divides it out before the 2x2 sum:
passes 13 (clear), 10 (block sums into the green image), 11 (statistics on an odd-stride
lattice, shared-memory accumulation then global atomics), 12 (curve per block type), and
the fit branch of pass 6. Statistics and curve live in the diff buffer. CPU reference:
`imaging/PhaseFit.h`; tests `phase_fit_test` and the `reduce-detail-fit` cases of
`raw_remosaic_vulkan_test`. Samples must be <= 4095. The fit is per capture, never reuse
a curve across ISO/modes.
`Frame::gainField` (quad block=2, RAW16, no reduction) divides a per-position gain field out
while the raw is loaded: pass 14 replaces the transfer copy, reading the camera rows and
writing each sample divided by its position's quadratic surface (coefficients copied into the
diff buffer with vkCmdUpdateBuffer; `fieldRow0`/`fieldHeight` place a strip window in its
frame). The field is fitted on the CPU from the frame's own flat blocks (`imaging/QuadGainField.h`,
which also holds the CPU apply); `raw_remosaic_vulkan_test` checks strips against the whole
frame and the CPU (`... OUT field` for a real 10-bit frame).
The reduction is used by capture through `capture/ReducedRaw` (lens `photoOutput`).

For 16320x12288 input, scratch consists of two 8160x6144 R16 images and one
4080x3072 vec2 buffer: 300,810,240 logical bytes, excluding image allocation
padding, input, destination, and readback. There is no 200MP scratch image.
This is allocation arithmetic, not measured peak memory.

## Bayer downscales (`BayerBinner`)

`BayerBinner::bin()` (`shaders/bayer_lin.comp`) halves a Bayer frame per side (200 MP -> 50 MP), RAW16
rows in a storage buffer to an R16_UINT image, codes scaled by `codeGain` (4 in capture), in one pass.
Each output pixel is a fixed 12x12 weighted sum of the white-balanced source around it, one weight set
per output site, fitted by least squares (`tools/bayer_bin_fit/fit_linear_bin.py`) to an anti-aliased
2x downscale with the 200 MP sensors' 4-pixel grid lines and 8x8 diagonal grid nulled exactly, a
penalty on any response beyond the 50 MP Nyquist, and each colour reproducing itself (and nothing of
the others) below 0.12 cycles per source pixel, so narrow coloured detail keeps its colour. The result
is clamped to the 3x3 same-colour source samples around the output (taken from the source rows already
in registers), so the kernel cannot ring past an edge. No sharpening: the renderer's capture
sharpening owns that, and sharpening here stacks with it into halos. Weights live in
`include/rawr/raw_ingress/BayerBinKernels.h` (generated by `tools/bayer_bin_fit/export_kernels.py`,
uploaded as a uniform buffer and staged in shared memory: a large constant array indexed at run time is
slow on Adreno). Other Bayer orders use the RGGB weights mirrored. No scratch. `BayerBinNoise.h` gives
the flat-area noise variance per output colour for the capture's noise profile.

Against the natural bin it replaces in capture (the renderer and the DNG converter keep the CPU one),
on a full F85 frame through the offline rawr render: about 1.4x the mid/high detail, no 8x8 diagonal
grid (the natural bin leaves a peak 15-30 dB above its surroundings in sky and foliage), the same flat
noise, thin coloured detail at the natural bin's saturation, and almost no edge overshoot. Timing for a
200 MP frame: about 0.14 s on the Adreno 840 phone (natural bin 0.89 s), 0.12-0.13 s on an M5 under
MoltenVK.

With `Bin::ratio = 3` (`shaders/bayer_bin3.comp`) the same method bins 3x per side in one pass (200 MP
-> 5440x4096, 22 MP; `outputSize()` / `BayerBinSize.h`: a third per side rounded down to a multiple of 8,
from the centred source area). Output pixel X is centred on source pixel 3X+1, so the 17x17 windows are
symmetric about a source pixel and other Bayer orders use the RGGB weights of the phase-shifted output
site (no mirroring). On the F85 frame it keeps 98% / 83% of the mid / high detail of the 50 MP render
resized to the same size, the same as an ideal 22 MP Bayer raw (99% / 85%); the bin + resample route to
25 MP kept 64% / 19%, because resampling each colour plane on its own cannot carry luma detail. About
0.21 s per 200 MP frame on the Adreno 840 (as long as the old bin + resample), 0.04 s on an M5. On the
Adreno, two outputs per thread, 32x16 tiles, an odd shared-memory row stride (bank conflicts halved the
speed) and unrolled inner loops (1.3x) matter.

`BayerBinner::resample()` (`shaders/bayer_resample.comp`) is the converter's `BayerResample`
(50 MP -> 25 MP), used after the 4x4 reduction. `tests/native/bayer_bin_vulkan_test.cpp` checks the bins
and the resample against CPU references (the 3x one picks each output's RGGB weights from the window's
colours, independently of the shader's phase rule, and flat colours must come out exact in every order);
with `- 16320 12288 0` it times a synthetic 200 MP frame.

Build `raw_remosaic_vulkan_test` via the tests CMake project. With no arguments
it covers both legacy paths and new High/reduction paths. Optional arguments:
`RAW16 WIDTH HEIGHT CFA_ORDER OUTPUT_RAW16` run a full input against CPU
and write the GPU quarter-DN Bayer output. RAW16 files are little-endian
unpacked codes. See `tools/hexadeca_reference/README.md` for results.
