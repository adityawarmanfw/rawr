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
