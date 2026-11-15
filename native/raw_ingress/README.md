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

## Standalone Hexadeca reduction (not wired into capture)

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
None of the new options is selected by app capture/merge code.

For 16320x12288 input, scratch consists of two 8160x6144 R16 images and one
4080x3072 vec2 buffer: 300,810,240 logical bytes, excluding image allocation
padding, input, destination, and readback. There is no 200MP scratch image.
This is allocation arithmetic, not measured peak memory.

Build `raw_remosaic_vulkan_test` via the tests CMake project. With no arguments
it covers both legacy paths and new High/reduction paths. Optional arguments:
`RAW16 WIDTH HEIGHT CFA_ORDER OUTPUT_RAW16` run a full input against CPU
and write the GPU quarter-DN Bayer output. RAW16 files are little-endian
unpacked codes. See `tools/hexadeca_reference/README.md` for results.
